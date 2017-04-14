// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright 2017 University of Massachusetts, Amherst

#ifndef MESH__GLOBALMESHINGHEAP_H
#define MESH__GLOBALMESHINGHEAP_H

#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include "heaplayers.h"

#include "internal.h"
#include "meshable-arena.h"
#include "meshing.h"
#include "miniheap.h"

using namespace HL;

namespace mesh {

template <int NumBins>
class GlobalHeapStats {
public:
  atomic_size_t meshCount;
  atomic_size_t mhFreeCount;
  atomic_size_t mhAllocCount;
  atomic_size_t mhHighWaterMark;
  atomic_size_t mhClassHWM[NumBins];
};

template <typename BigHeap,                      // for large allocations
          int NumBins,                           // number of size classes
          int (*getSizeClass)(const size_t),     // same as for local
          size_t (*getClassMaxSize)(const int),  // same as for local
          int MeshPeriod,                        // perform meshing on average once every MeshPeriod frees
          size_t MinStringLen = 8UL>
class GlobalMeshingHeap : public MeshableArena {
private:
  DISALLOW_COPY_AND_ASSIGN(GlobalMeshingHeap);
  typedef MeshableArena Super;

  static_assert(getClassMaxSize(NumBins - 1) == 16384, "expected 16k max object size");
  static_assert(HL::gcd<BigHeap::Alignment, Alignment>::value == Alignment,
                "expected BigHeap to have 16-byte alignment");

public:
  enum { Alignment = 16 };

  GlobalMeshingHeap() : _maxObjectSize(getClassMaxSize(NumBins - 1)), _prng(internal::seed()) {
    for (size_t i = 0; i < NumBins; ++i) {
      _littleheapCounts[i] = 0;
      _littleheaps[i] = nullptr;
    }
    resetNextMeshCheck();
  }

  inline void dumpStats(int level) const {
    if (level < 1)
      return;

    debug("MESH COUNT:         %zu\n", (size_t)_stats.meshCount);
    debug("MH Alloc Count:     %zu\n", (size_t)_stats.mhAllocCount);
    debug("MH Free  Count:     %zu\n", (size_t)_stats.mhFreeCount);
    debug("MH High Water Mark: %zu\n", (size_t)_stats.mhHighWaterMark);
    for (size_t i = 0; i < NumBins; i++) {
      auto size = getClassMaxSize(i);
      if (_littleheapCounts[i] == 0) {
        debug("MH HWM (%5zu):     %zu\n", size, (size_t)_stats.mhClassHWM[i]);
        continue;
      }
      auto objectCount = _littleheaps[i]->maxCount() * _littleheapCounts[i];
      double inUseCount = 0;
      for (auto mh = _littleheaps[i]; mh != nullptr; mh = mh->next()) {
        inUseCount += mh->inUseCount();
      }
      debug("MH HWM (%5zu):     %zu (occ: %f)\n", size, (size_t)_stats.mhClassHWM[i], inUseCount / objectCount);
    }
  }

  inline MiniHeap *allocMiniheap(size_t objectSize) {
    std::unique_lock<std::shared_timed_mutex> exclusiveLock(_mhRWLock);

    d_assert(objectSize <= _maxObjectSize);

    const int sizeClass = getSizeClass(objectSize);
    const size_t sizeMax = getClassMaxSize(sizeClass);

    d_assert_msg(objectSize == sizeMax, "sz(%zu) shouldn't be greater than %zu (class %d)", objectSize, sizeMax,
                 sizeClass);
    d_assert(sizeClass >= 0);
    d_assert(sizeClass < NumBins);

    // if we have objects bigger than the size of a page, allocate
    // multiple pages to amortize the cost of creating a
    // miniheap/globally locking the heap.
    size_t nObjects = max(HL::CPUInfo::PageSize / sizeMax, MinStringLen);

    const size_t nPages = PageCount(sizeMax * nObjects);
    const size_t spanSize = HL::CPUInfo::PageSize * nPages;
    d_assert(0 < spanSize);

    void *span = Super::malloc(spanSize);
    if (unlikely(span == nullptr))
      abort();

    void *buf = internal::Heap().malloc(sizeof(MiniHeap));
    if (unlikely(buf == nullptr))
      abort();
    MiniHeap *mh = new (buf) MiniHeap(span, nObjects, sizeMax, _prng, spanSize);

    trackMiniheap(sizeClass, mh);
    _miniheaps[mh->getSpanStart()] = mh;

    _stats.mhAllocCount++;
    _stats.mhHighWaterMark = max(_miniheaps.size(), _stats.mhHighWaterMark.load());
    _stats.mhClassHWM[sizeClass] = max(_littleheapCounts[sizeClass], _stats.mhClassHWM[sizeClass].load());

    return mh;
  }

  inline void *malloc(size_t sz) {
    const int sizeClass = getSizeClass(sz);
    const size_t sizeMax = getClassMaxSize(sizeClass);

    if (unlikely(sizeMax <= _maxObjectSize))
      abort();

    std::lock_guard<std::mutex> lock(_bigMutex);
    return _bigheap.malloc(sz);
  }

  inline MiniHeap *miniheapFor(void *const ptr) const {
    std::shared_lock<std::shared_timed_mutex> sharedLock(_mhRWLock);

    const auto ptrval = reinterpret_cast<uintptr_t>(ptr);
    auto it = greatest_leq(_miniheaps, ptrval);
    if (likely(it != _miniheaps.end())) {
      auto candidate = it->second;
      if (likely(candidate->contains(ptr)))
        return candidate;
    }

    return nullptr;
  }

  void trackMiniheap(size_t sizeClass, MiniHeap *mh) {
    _littleheapCounts[sizeClass] += 1;
    if (_littleheaps[sizeClass] == nullptr)
      _littleheaps[sizeClass] = mh;
    else
      _littleheaps[sizeClass]->insertNext(mh);
  }

  void untrackMiniheap(size_t sizeClass, MiniHeap *mh) {
    _littleheapCounts[sizeClass] -= 1;

    auto next = mh->remove();
    if (_littleheaps[sizeClass] == mh)
      _littleheaps[sizeClass] = next;
  }

  // called with lock held
  void freeMiniheapAfterMesh(MiniHeap *mh) {
    const auto sizeClass = getSizeClass(mh->objectSize());
    untrackMiniheap(sizeClass, mh);

    mh->MiniHeap::~MiniHeap();
    internal::Heap().free(mh);
  }

  void freeMiniheap(MiniHeap *&mh) {
    std::unique_lock<std::shared_timed_mutex> exclusiveLock(_mhRWLock);

    const auto spans = mh->spans();
    const auto spanSize = mh->spanSize();

    const auto meshCount = mh->meshCount();
    for (size_t i = 0; i < meshCount; i++) {
      Super::free(reinterpret_cast<void *>(spans[i]), spanSize);
      _miniheaps.erase(reinterpret_cast<uintptr_t>(spans[i]));
    }

    _stats.mhFreeCount++;

    freeMiniheapAfterMesh(mh);
    mh = nullptr;
  }

  inline void free(void *ptr) {
    if (unlikely(internal::isMeshMarker(ptr))) {
      dumpStats(2);
      for (size_t i = 0; i < 128; i++)
        meshAllSizeClasses();
      dumpStats(2);
      return;
    }

    // two possibilities: most likely the ptr is small (and therefor
    // owned by a miniheap), or is a large allocation

    auto mh = miniheapFor(ptr);
    if (likely(mh)) {
      mh->free(ptr);
      if (unlikely(mh->isDone() && mh->isEmpty())) {
        freeMiniheap(mh);
      }
      //  else if (unlikely(shouldMesh())) {
      //   meshAllSizeClasses();
      // }
    } else {
      std::lock_guard<std::mutex> lock(_bigMutex);
      _bigheap.free(ptr);
    }
  }

  inline size_t getSize(void *ptr) const {
    if (ptr == nullptr || internal::isMeshMarker(ptr))
      return 0;

    auto mh = miniheapFor(ptr);
    if (mh) {
      return mh->getSize(ptr);
    } else {
      std::lock_guard<std::mutex> lock(_bigMutex);
      return _bigheap.getSize(ptr);
    }
  }

  // must be called with the world stopped, after call to mesh()
  // completes src is a nullptr
  void mesh(MiniHeap *dst, MiniHeap *&src) {
    // FIXME: dst might have a few spans
    const auto srcSpan = src->getSpanStart();
    auto objectSize = dst->objectSize();

    size_t sz = dst->spanSize();

    // for each object in src, copy it to dst + update dst's bitmap
    // and in-use count
    for (auto const &off : src->bitmap()) {
      d_assert(!dst->bitmap().isSet(off));
      void *srcObject = reinterpret_cast<void *>(srcSpan + off * objectSize);
      void *dstObject = dst->mallocAt(off);
      d_assert(dstObject != nullptr);
      memcpy(dstObject, srcObject, objectSize);
    }

    dst->meshedSpan(srcSpan);
    Super::mesh(reinterpret_cast<void *>(dst->getSpanStart()), reinterpret_cast<void *>(src->getSpanStart()), sz);

    std::unique_lock<std::shared_timed_mutex> exclusiveLock(_mhRWLock);
    freeMiniheapAfterMesh(src);
    src = nullptr;

    _miniheaps[srcSpan] = dst;
  }

  size_t getAllocatedMiniheapCount() const {
    return Super::bitmap().inUseCount();
  }

  void lock() {
    _mhRWLock.lock();
    _bigMutex.lock();
  }

  void unlock() {
    _bigMutex.unlock();
    _mhRWLock.unlock();
  }

protected:
  inline void resetNextMeshCheck() {
    uniform_int_distribution<size_t> distribution(1, MeshPeriod);
    _nextMeshCheck = distribution(_prng);
  }

  inline bool shouldMesh() {
    _nextMeshCheck--;
    bool shouldMesh = _nextMeshCheck == 0;
    if (unlikely(shouldMesh))
      resetNextMeshCheck();

    return shouldMesh;
  }

  // check for meshes in all size classes
  void meshAllSizeClasses() {
    internal::vector<internal::vector<MiniHeap *>> mergeSets;

    // FIXME: is it safe to have this function not use internal::allocator?
    auto meshFound = function<void(internal::vector<MiniHeap *> &&)>(
        // std::allocator_arg, internal::allocator,
        [&](internal::vector<MiniHeap *> &&mesh) { mergeSets.push_back(std::move(mesh)); });

    debug("TODO: re-enable meshing");
    // for (const auto &miniheaps : _littleheaps) {
    //   randomSort(_prng, miniheaps, meshFound);
    // }

    if (mergeSets.size() == 0)
      return;

    _stats.meshCount += mergeSets.size();

    internal::StopTheWorld();

    for (auto &mergeSet : mergeSets) {
      d_assert(mergeSet.size() == 2);  // FIXME
      mesh(mergeSet[0], mergeSet[1]);
    }

    internal::StartTheWorld();
  }

  const size_t _maxObjectSize;
  size_t _nextMeshCheck{0};

  // The big heap is handles malloc requests for large objects.  We
  // define a separate class to handle these to segregate bookkeeping
  // for large malloc requests from the ones used to back spans (which
  // are allocated from the arena)
  BigHeap _bigheap{};

  MiniHeap *_current[NumBins];

  mt19937_64 _prng;

  size_t _littleheapCounts[NumBins];
  MiniHeap *_littleheaps[NumBins];
  internal::map<uintptr_t, MiniHeap *> _miniheaps{};

  mutable std::mutex _bigMutex{};
  mutable std::shared_timed_mutex _mhRWLock{};

  GlobalHeapStats<NumBins> _stats{};
};
}  // namespace mesh

#endif  // MESH__GLOBALMESHINGHEAP_H
