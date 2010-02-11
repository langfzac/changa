#ifndef __STATE_H__
#define __STATE_H__
#include "ParallelGravity.h"

// less flexible, but probably more efficient
// and allows for sharing of counters between
// states
class State {
  public:
    int bWalkDonePending; // needed for combiner cache flushes
    // this variable is used instead of TreePiece::currentPrefetch
    // in the prefetch walk
    int currentBucket;  // The bucket we have started to walk.

    // shifted variable into state. there is an issue of redundancy 
    // here, though. in addition to local state, remote and remote-resume
    // state also have this variable but have no use for it, since only
    // a single copy is required.
    // could have made this the third element in the array below
    int myNumParticlesPending;

    // again, redundant variables, since only remote-no-resume
    // walks use this variable to see how many chunks have 
    // been used
    int numPendingChunks;

    // posn 0: bucket requests
    // posn 1: chunk requests
    int *counterArrays[2];
    virtual ~State() {}
};

#if INTERLIST_VER > 0
#if defined CUDA
#include "HostCUDA.h"
#include "DataManager.h"

class DoubleWalkState;

template<typename T>
class GenericList{
  public:
  CkVec<CkVec<T> > lists;
  int totalNumInteractions;

  GenericList() : totalNumInteractions(0) {}

  void reset(){
    // clear all bucket lists:
    for(int i = 0; i < lists.length(); i++){
      lists[i].length() = 0;
    }
    totalNumInteractions = 0;
  }

  void free(){
    for(int i = 0; i < lists.length(); i++){
      lists[i].free();
    }
    lists.free();
    totalNumInteractions = 0;
  }

  void init(int numBuckets, int numper){
    lists.resize(numBuckets);
    for(int i = 0; i < numBuckets; i++){
      lists[i].reserve(numper);
    }
  }

  CudaRequest *serialize(TreePiece *tp);
  void getBucketParameters(TreePiece *tp, 
                           int bucket, 
                           int &bucketStart, int &bucketSize){
                           //std::map<NodeKey, int>&lpref){
	// bucket is listed in this offload
	GenericTreeNode *bucketNode = tp->bucketList[bucket];

	bucketSize = bucketNode->lastParticle - bucketNode->firstParticle + 1;
        bucketStart = bucketNode->bucketArrayIndex;
	CkAssert(bucketStart >= 0);
  }

  void getActiveBucketParameters(TreePiece *tp, 
                           int bucket, 
                           int &bucketStart, int &bucketSize){
                           //std::map<NodeKey, int>&lpref){
	// bucket is listed in this offload
	GenericTreeNode *bucketNode = tp->bucketList[bucket];
        BucketActiveInfo *binfo = &(tp->bucketActiveInfo[bucket]);

	//bucketSize = bucketNode->lastParticle - bucketNode->firstParticle + 1;
        //bucketStart = bucketNode->bucketArrayIndex;
        bucketSize = tp->bucketActiveInfo[bucket].size;
        bucketStart = tp->bucketActiveInfo[bucket].start;
	CkAssert(bucketStart >= 0);
  }

  void push_back(int b, T &ilc, DoubleWalkState *state, TreePiece *tp);
  

};

#endif

class DoubleWalkState : public State {
  public:
  CheckList *chklists;
  UndecidedLists undlists;
  CkVec<CkVec<OffsetNode> >clists;
  CkVec<CkVec<LocalPartInfo> >lplists;
  CkVec<CkVec<RemotePartInfo> >rplists;
   
  // set once before the first cgr is called for a chunk
  // the idea is to place the chunkRoot (along with replicas)
  // on the remote comp chklist only once per chunk
  //
  // one for each chunk
  bool *placedRoots;
  // to tell a remote-resume state from a remote-no-resume state
  bool resume;

#ifdef CUDA
  int nodeThreshold;
  int partThreshold;

  GenericList<ILCell> nodeLists;
  GenericList<ILPart> particleLists;

  CkVec<CudaMultipoleMoments> *nodes;
  CkVec<CompactPartData> *particles;

  // during 'small' rungs, buckets are marked when
  // they are included for computation in the request's
  // aux. particle array. these markings should be
  // cleared before the assembly of the next request is
  // begun. for this purpose, we keep track of buckets
  // marked during the construction of a request.
  //
  // NB: for large rungs, we don't mark buckets while 
  // compiling requests. for such rungs, since all
  // particles are shipped at the beginning of the iteration,
  // we have them marked at that time. since all particles,
  // are available on the gpu for these rungs, we do not clear 
  // the markings when requests are sent out.
  CkVec<GenericTreeNode *> markedBuckets;

  // TODO : this switch from map to ckvec means that we cannot 
  // use multiple treepieces per processor, since they will all
  // be writing to the nodeArrayIndex field of the CacheManager's nodes.
  // We need a different group that manages GPU memory for this purpose.
  //std::map<NodeKey,int> nodeMap;
  CkVec<GenericTreeNode *> nodeMap;
  std::map<NodeKey,int> partMap;

  bool nodeOffloadReady(){
    return nodeLists.totalNumInteractions >= nodeThreshold;
  }

  bool partOffloadReady(){
    return particleLists.totalNumInteractions >= partThreshold;
  }
#endif

  // The lowest nodes reached on paths to each bucket
  // Used to find numBuckets completed when
  // walk returns. Also used to find at which
  // bucket computation should start
  GenericTreeNode *lowestNode;
  int level;

  DoubleWalkState() : chklists(0), lowestNode(0), level(-1)
  {}

};


#if defined CUDA
void allocatePinnedHostMemory(void **ptr, int size);

template<typename T>
CudaRequest *GenericList<T>::serialize(TreePiece *tp){
    // get count of buckets with interactions first
    int numFilledBuckets = 0;
    int listpos = 0;
    int curbucket = 0;

    double starttime = CmiWallTimer();
    for(int i = 0; i < lists.length(); i++){
      if(lists[i].length() > 0){
        numFilledBuckets++;
      }
    }

    // create flat lists and associated data structures
    // allocate memory and flatten lists only if there
    // are interactions to transfer. 
    T *flatlists = NULL;
    int *markers = NULL;
    int *starts = NULL;
    int *sizes = NULL;
    int *affectedBuckets = NULL;

    if(totalNumInteractions > 0){
#ifdef CUDA_USE_CUDAMALLOCHOST
      allocatePinnedHostMemory((void **)&flatlists, totalNumInteractions*sizeof(T));
      allocatePinnedHostMemory((void **)&markers, (numFilledBuckets+1)*sizeof(int));
      allocatePinnedHostMemory((void **)&starts, (numFilledBuckets)*sizeof(int));
      allocatePinnedHostMemory((void **)&sizes, (numFilledBuckets)*sizeof(int));
#else
      flatlists = (T *) malloc(totalNumInteractions*sizeof(T));
      markers = (int *) malloc((numFilledBuckets+1)*sizeof(int));
      starts = (int *) malloc(numFilledBuckets*sizeof(int));
      sizes = (int *) malloc(numFilledBuckets*sizeof(int));
#endif
      affectedBuckets = new int[numFilledBuckets];

      // populate flat lists
      int listslen = lists.length();
      if(tp->largePhase()){
        for(int i = 0; i < listslen; i++){
          int listilen = lists[i].length();
          if(listilen > 0){
            memcpy(&flatlists[listpos], lists[i].getVec(), listilen*sizeof(T));
            markers[curbucket] = listpos;
            getBucketParameters(tp, i, starts[curbucket], sizes[curbucket]);
            affectedBuckets[curbucket] = i;
            listpos += listilen;
            curbucket++;
          }
        }
      }
      else{
        for(int i = 0; i < listslen; i++){
          int listilen = lists[i].length();
          if(listilen > 0){
            memcpy(&flatlists[listpos], lists[i].getVec(), listilen*sizeof(T));
            markers[curbucket] = listpos;
            getActiveBucketParameters(tp, i, starts[curbucket], sizes[curbucket]);
            affectedBuckets[curbucket] = i;
            listpos += listilen;
            curbucket++;
          }
        }
      }
      markers[numFilledBuckets] = listpos;
      CkAssert(listpos == totalNumInteractions);
    }

    CudaRequest *request = new CudaRequest;
    request->list = (void *)flatlists;
    request->bucketMarkers = markers;
    request->bucketStarts = starts;
    request->bucketSizes = sizes;
    request->numInteractions = totalNumInteractions;
    request->numBucketsPlusOne = numFilledBuckets+1;
    request->affectedBuckets = affectedBuckets;
    request->tp = (void *)tp;
    request->fperiod = tp->fPeriod.x;

    traceUserBracketEvent(CUDA_SER_LIST, starttime, CmiWallTimer());

    return request;
  }

//#include <typeinfo>
template<typename T>
void GenericList<T>::push_back(int b, T &ilc, DoubleWalkState *state, TreePiece *tp){
    if(lists[b].length() == 0){
        state->counterArrays[0][b]++;
#if COSMO_PRINT_BK > 1
        CkPrintf("[%d] request out bucket %d numAddReq: %d,%d\n", tp->getIndex(), b, tp->sRemoteGravityState->counterArrays[0][b], tp->sLocalGravityState->counterArrays[0][b]);
#endif
    }
    lists[b].push_back(ilc);
    totalNumInteractions++;
  }
#endif // CUDA
#endif //  INTERLIST_VER 

class NullState : public State {
};

class ListState : public State {
  //public:
  //OffsetNode nodeList;
  //CkVec<LocalPartInfo> localParticleList;
  //CkVec<RemotePartInfo> remoteParticleList;
};

#endif
