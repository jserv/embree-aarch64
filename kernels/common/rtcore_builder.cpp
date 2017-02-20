// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#ifdef _WIN32
#  define RTCORE_API extern "C" __declspec(dllexport)
#else
#  define RTCORE_API extern "C" __attribute__ ((visibility ("default")))
#endif

#include "default.h"
#include "device.h"
#include "scene.h"
#include "context.h"
#include "alloc.h"

#include "../builders/bvh_builder_sah.h"
#include "../builders/bvh_builder_morton.h"

namespace embree
{ 
  namespace isa // FIXME: support more ISAs for builders
  {
    struct BVH
    {
      BVH (Device* device)
        : device(device), isStatic(false), allocator(device,true), morton_src(device), morton_tmp(device) {}

    public:
      Device* device;
      bool isStatic;
      FastAllocator allocator;
      mvector<BVHBuilderMorton::BuildPrim> morton_src;
      mvector<BVHBuilderMorton::BuildPrim> morton_tmp;
    };

    RTCORE_API RTCBVH rtcNewBVH(RTCDevice device)
    {
      RTCORE_CATCH_BEGIN;
      RTCORE_TRACE(rtcNewAllocator);
      RTCORE_VERIFY_HANDLE(device);
      return (RTCBVH) new BVH((Device*)device);
      RTCORE_CATCH_END((Device*)device);
      return nullptr;
    }

    RTCORE_API void* rtcThreadLocalAlloc(RTCThreadLocalAllocator localAllocator, size_t bytes, size_t align)
    {
      RTCORE_CATCH_BEGIN;
      RTCORE_TRACE(rtcThreadLocalAlloc);
      FastAllocator::ThreadLocal* alloc = (FastAllocator::ThreadLocal*) localAllocator;
      return alloc->malloc(bytes,align);
      RTCORE_CATCH_END(((FastAllocator::ThreadLocal*) localAllocator)->alloc->getDevice());
      return nullptr;
    }

    RTCORE_API void rtcMakeStaticBVH(RTCBVH hbvh)
    {
      BVH* bvh = (BVH*) hbvh;
      RTCORE_CATCH_BEGIN;
      RTCORE_TRACE(rtcStaticBVH);
      RTCORE_VERIFY_HANDLE(hbvh);
      bvh->allocator.shrink();
      bvh->morton_src.clear();
      bvh->morton_tmp.clear();
      bvh->isStatic = true;
      RTCORE_CATCH_END(bvh->device);
    }

    RTCORE_API void rtcDeleteBVH(RTCBVH hbvh)
    {
      BVH* bvh = (BVH*) hbvh;
      Device* device = bvh ? bvh->device : nullptr;
      RTCORE_CATCH_BEGIN;
      RTCORE_TRACE(rtcDeleteAllocator);
      RTCORE_VERIFY_HANDLE(hbvh);
      delete bvh;
      RTCORE_CATCH_END(device);
    }

    RTCORE_API void* rtcBVHBuildSAH(RTCBVH hbvh,
                                    const RTCBuildSettings& settings,
                                    RTCBuildPrimitive* prims,
                                    size_t numPrims,
                                    void* userPtr,
                                    RTCCreateNodeFunc createNode,
                                    RTCSetNodeChildFunc setNodeChild,
                                    RTCSetNodeBoundsFunc setNodeBounds,
                                    RTCCreateLeafFunc createLeaf,
                                    RTCBuildProgressFunc buildProgress)
    {
      BVH* bvh = (BVH*) hbvh;
      RTCORE_CATCH_BEGIN;
      RTCORE_TRACE(rtcBVHBuildSAH);
      RTCORE_VERIFY_HANDLE(hbvh);

      if (bvh->isStatic)
        throw_RTCError(RTC_INVALID_OPERATION,"static BVH cannot get rebuild");

      bvh->allocator.init_estimate(numPrims*sizeof(BBox3fa));
      bvh->allocator.reset();

      /* calculate priminfo */
      auto computeBounds = [&](const range<size_t>& r) -> CentGeomBBox3fa
        {
          CentGeomBBox3fa bounds(empty);
          for (size_t j=r.begin(); j<r.end(); j++)
            bounds.extend((BBox3fa&)prims[j]);
          return bounds;
        };
      const CentGeomBBox3fa bounds = 
        parallel_reduce(size_t(0),numPrims,size_t(1024),size_t(1024),CentGeomBBox3fa(empty), computeBounds, CentGeomBBox3fa::merge2);

      const PrimInfo pinfo(0,numPrims,bounds.geomBounds,bounds.centBounds);
      
      /* build BVH */
      void* root = BVHBuilderBinnedSAH::build<void*>(
        
        /* thread local allocator for fast allocations */
        [&] () -> FastAllocator::ThreadLocal* { 
          return bvh->allocator.threadLocal();
        },

        /* lambda function that creates BVH nodes */
        [&](BVHBuilderBinnedSAH::BuildRecord* children, const size_t N, FastAllocator::ThreadLocal* alloc) -> void*
        {
          void* node = createNode((RTCThreadLocalAllocator)alloc,N,userPtr);
          for (size_t i=0; i<N; i++)
            setNodeBounds(node,i,(RTCBounds&)children[i].prims.geomBounds,userPtr);
          return node;
        },

        /* lambda function that updates BVH nodes */
        [&](void* node, void** children, const size_t N) -> void*
        {
          for (size_t i=0; i<N; i++)
            setNodeChild(node,i,children[i],userPtr);
          return node;
        },
        
        /* lambda function that creates BVH leaves */
        [&](const BVHBuilderBinnedSAH::BuildRecord& current, FastAllocator::ThreadLocal* alloc) -> void* {
          return createLeaf((RTCThreadLocalAllocator)alloc,prims+current.prims.begin(),current.prims.size(),userPtr);
        },
        
        /* progress monitor function */
        [&] (size_t dn) { 
          buildProgress(dn,userPtr);
        },
        
        (PrimRef*)prims,pinfo,settings);
        
      bvh->allocator.cleanup();
      return root;

      RTCORE_CATCH_END(bvh->device);
      return nullptr;
    }

    RTCORE_API void* rtcBVHBuildMorton(RTCBVH hbvh,
                                       const RTCBuildSettings& settings,
                                       RTCBuildPrimitive* prims_i,
                                       size_t numPrims,
                                       void* userPtr,
                                       RTCCreateNodeFunc createNode,
                                       RTCSetNodeChildFunc setNodeChild,
                                       RTCSetNodeBoundsFunc setNodeBounds,
                                       RTCCreateLeafFunc createLeaf,
                                       RTCBuildProgressFunc buildProgress)
    {
      BVH* bvh = (BVH*) hbvh;
      RTCORE_CATCH_BEGIN;
      RTCORE_TRACE(rtcBVHBuildMorton);
      RTCORE_VERIFY_HANDLE(hbvh);

      if (bvh->isStatic)
        throw_RTCError(RTC_INVALID_OPERATION,"static BVH cannot get rebuild");

      bvh->allocator.init_estimate(numPrims*sizeof(BBox3fa));
      bvh->allocator.reset();
      
      /* initialize temporary arrays for morton builder */
      PrimRef* prims = (PrimRef*) prims_i;
      mvector<BVHBuilderMorton::BuildPrim>& morton_src = bvh->morton_src;
      mvector<BVHBuilderMorton::BuildPrim>& morton_tmp = bvh->morton_tmp;
      morton_src.resize(numPrims);
      morton_tmp.resize(numPrims);
      parallel_for(size_t(0), numPrims, size_t(1024), [&] ( range<size_t> r ) {
          for (size_t i=r.begin(); i<r.end(); i++)
            morton_src[i].index = i;
        });
      
      /* start morton build */
      std::pair<void*,BBox3fa> root = BVHBuilderMorton::build<std::pair<void*,BBox3fa>>(
        
        /* thread local allocator for fast allocations */
        [&] () -> FastAllocator::ThreadLocal* { 
          return bvh->allocator.threadLocal();
        },
        
        /* lambda function that allocates BVH nodes */
        [&] ( FastAllocator::ThreadLocal* alloc, size_t N ) -> void* {
          return createNode((RTCThreadLocalAllocator)alloc,N,userPtr);
        },
        
        /* lambda function that sets bounds */
        [&] (void* node, const std::pair<void*,BBox3fa>* children, size_t N) -> std::pair<void*,BBox3fa>
        {
          BBox3fa bounds = empty;
          for (size_t i=0; i<N; i++) {
            bounds.extend(children[i].second);
            setNodeChild(node,i,children[i].first,userPtr);
            setNodeBounds(node,i,(RTCBounds&)children[i].second,userPtr);
          }
          return std::make_pair(node,bounds);
        },
        
        /* lambda function that creates BVH leaves */
        [&]( const range<unsigned>& current, FastAllocator::ThreadLocal* alloc) -> std::pair<void*,BBox3fa>
        {
          const size_t id = morton_src[current.begin()].index;
          const BBox3fa bounds = prims[id].bounds(); 
          void* node = createLeaf((RTCThreadLocalAllocator)alloc,prims_i+current.begin(),current.size(),userPtr);
          return std::make_pair(node,bounds);
        },
        
        /* lambda that calculates the bounds for some primitive */
        [&] (const BVHBuilderMorton::BuildPrim& morton) -> BBox3fa {
          return prims[morton.index].bounds();
        },
        
        /* progress monitor function */
        [&] (size_t dn) { 
          buildProgress(dn,userPtr);
        },
        
        morton_src.data(),morton_tmp.data(),numPrims,
        settings);

      bvh->allocator.cleanup();
      return root.first;

      RTCORE_CATCH_END(bvh->device);
      return nullptr;
    }
  }
}
