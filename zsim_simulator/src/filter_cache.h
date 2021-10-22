/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FILTER_CACHE_H_
#define FILTER_CACHE_H_

#include "bithacks.h"
#include "cache.h"
#include "galloc.h"
#include "zsim.h"
#include "clu_stats.h"
#include "ma_prof.h"
#include "virtual_mem.h"

/* Extends Cache with an L0 direct-mapped cache, optimized to hell for hits
 *
 * L1 lookups are dominated by several kinds of overhead (grab the cache locks,
 * several virtual functions for the replacement policy, etc.). This
 * specialization of Cache solves these issues by having a filter array that
 * holds the most recently used line in each set. Accesses check the filter array,
 * and then go through the normal access path. Because there is one line per set,
 * it is fine to do this without grabbing a lock.
 */

class FilterCache : public Cache {
    private:
        struct FilterEntry {
            volatile Address rdAddr;
            volatile Address wrAddr;
            volatile uint64_t availCycle;

#ifdef CLU_STATS_ENABLED
            volatile CacheLineAccessMask_t accessMask;
            volatile uint64_t data[16]; // 8B * 8 = 64B cache line
#endif
            void clear() {
                wrAddr = UNDEF_CACHE_LINE_ADDRESS; rdAddr = UNDEF_CACHE_LINE_ADDRESS; availCycle = 0;
#ifdef CLU_STATS_ENABLED
                accessMask = CLU_STATS_ZERO_MASK;
#endif
            }
        };

        //Replicates the most accessed line of each set in the cache
        FilterEntry* filterArray;
        Address setMask;
        uint32_t numSets;
        uint32_t srcId; //should match the core
        uint32_t reqFlags;

        lock_t filterLock;
        uint64_t fGETSHit, fGETXHit;
#ifdef CLU_STATS_ENABLED
        uint64_t fCLEI;
        uint64_t fUCLC;
#endif

    public:
        FilterCache(uint32_t _numSets, CC* _cc, CacheArray* _array,
                ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, uint32_t _tagLat, g_string& _name)
            : Cache( _cc, _array, _rp, _accLat, _invLat, _tagLat, _name)
        {
            numSets = _numSets;
            setMask = numSets - 1;
            filterArray = gm_memalign<FilterEntry>(CACHE_LINE_BYTES, numSets);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            futex_init(&filterLock);
            fGETSHit = fGETXHit = 0;
#ifdef CLU_STATS_ENABLED
            fCLEI = 0;
            fUCLC = 0;
#endif
            srcId = -1;
            reqFlags = 0;
            assert(32 >= lineBits + MAX_MEMREQ_FLAGS + 1);
        }

        void setSourceId(uint32_t id) {
            srcId = id;
        }

        uint32_t getSourceId() {
            return srcId;
        }

        void setFlags(uint32_t flags) {
            reqFlags = flags;
        }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* cacheStat = new AggregateStat();
            cacheStat->init(name.c_str(), "Filter cache stats");

            ProxyStat* fgetsStat = new ProxyStat();
            fgetsStat->init("fhGETS", "Filtered GETS hits", &fGETSHit);
            ProxyStat* fgetxStat = new ProxyStat();
            fgetxStat->init("fhGETX", "Filtered GETX hits", &fGETXHit);
#ifdef CLU_STATS_ENABLED
            ProxyStat* fCLEIStat = new ProxyStat();
            fCLEIStat->init("fCLEI", "Filtered cache line evictions and invalidations", &fCLEI);
            ProxyStat*fUCLCStat = new ProxyStat();
            fUCLCStat->init("fUCLC", "Filtered utilized cache line chunks", &fUCLC);
#endif
            cacheStat->append(fgetsStat);
            cacheStat->append(fgetxStat);
#ifdef CLU_STATS_ENABLED
            cacheStat->append(fCLEIStat);
            cacheStat->append(fUCLCStat);
#endif

            initCacheStats(cacheStat);
            parentStat->append(cacheStat);
        }

#ifdef CLU_STATS_ENABLED
        inline void processAccessCLUStats(Address vAddr, MASize_t size, MemReqStatType_t memReqStatType, uint32_t lineIndex) {
            filterArray[lineIndex].accessMask |= cluStatsGetUtilizationMask(vAddr, size, memReqStatType);
        }
#endif

        inline uint64_t load(Address vAddr, uint64_t curCycle
#ifdef CLU_STATS_ENABLED
                             , MASize_t size, MemReqStatType_t memReqStatType
#endif
#ifdef MA_PROF_ENABLED
                             , PointerTag_t tag, MAOffset_t offset, Address bblIP
#endif
                             , uint32_t tid
                             ) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint32_t bytes = vAddr & ((1 << lineBits) - 1);
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            if (vLineAddr == filterArray[idx].rdAddr) {
#ifdef CLU_STATS_ENABLED
                processAccessCLUStats(vAddr, size, memReqStatType, idx);
#endif
                fGETSHit++;
                return MAX(curCycle, availCycle);
            } else {
                return replace(vLineAddr, idx, true, curCycle, vAddr
#ifdef CLU_STATS_ENABLED
                               , size, memReqStatType
#endif
#ifdef MA_PROF_ENABLED
                               , tag, offset, bblIP
#endif
                               , bytes, tid, false/*withZeroAlloc*/, 0
                               );
            }
        }

        inline uint64_t store(Address vAddr, uint64_t curCycle
#ifdef CLU_STATS_ENABLED
                              , MASize_t size
#endif
#ifdef MA_PROF_ENABLED
                              , PointerTag_t tag, MAOffset_t offset, Address bblIP
#endif
                              , uint32_t tid, uint64_t value
                              ) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint32_t bytes = vAddr & ((1 << lineBits) - 1);
            //info("TESTING vAddr %lx lineBits %u ((1 << lineBits) - 1) %x bytes %x", vAddr, lineBits, ((1 << lineBits) - 1), bytes);
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            if (vLineAddr == filterArray[idx].wrAddr) {
#ifdef CLU_STATS_ENABLED
                processAccessCLUStats(vAddr, size, StoreData, idx);
                uint32_t doubleWordIdx = bytes >> (uint32_t) log2(sizeof(uint64_t));
                assert(doubleWordIdx < 16);
                filterArray[idx].data[doubleWordIdx] = value;
#endif
                fGETXHit++;
                //NOTE: Stores don't modify availCycle; we'll catch matches in the core
                //filterArray[idx].availCycle = curCycle; //do optimistic store-load forwarding
                return MAX(curCycle, availCycle);
            } else {
                return replace(vLineAddr, idx, false, curCycle, vAddr
#ifdef CLU_STATS_ENABLED
                               , size, StoreData
#endif
#ifdef MA_PROF_ENABLED
                               , tag, offset, bblIP
#endif
                               , bytes, tid, false/*withZeroAlloc*/, 0
                               );
            }
        }

        uint64_t replace(Address vLineAddr, uint32_t idx, bool isLoad, uint64_t curCycle, Address vAddr
#ifdef CLU_STATS_ENABLED
                         , MASize_t size, MemReqStatType_t memReqStatType
#endif
#ifdef MA_PROF_ENABLED
                         , PointerTag_t tag, MAOffset_t offset, Address bblIP
#endif
                         , uint32_t bytes, uint32_t tid, bool withZeroAlloc, uint32_t zeroAllocLevel
                         ) {
            Address pLineAddr;
            uint64_t tlbLat = 0;
            if (zinfo->vm) {
                tlbLat = zinfo->vm->translate(vLineAddr, pLineAddr);
            } else {
                pLineAddr = procMask | vLineAddr;
            }
            MESIState dummyState = MESIState::I;
            futex_lock(&filterLock);

            //for zero alloc opt
            uint32_t localFlags = reqFlags;
            if (withZeroAlloc) {
                if (zeroAllocLevel == 1) {
                    localFlags |= MemReq::ZERO_ALLOC_1;
                } else if (zeroAllocLevel == 2) {
                    localFlags |= MemReq::ZERO_ALLOC_2;
                } else {
                    localFlags |= MemReq::ZERO_ALLOC_3;
                }
            }

            MemReq req = {pLineAddr, isLoad? GETS : GETX, 0, &dummyState, curCycle, &filterLock, dummyState, srcId,  localFlags | (bytes << (MAX_MEMREQ_FLAGS + 1))
#ifdef CLU_STATS_ENABLED
                    , {vAddr, size, memReqStatType, filterArray[idx].rdAddr, filterArray[idx].accessMask}
#endif
#ifdef MA_PROF_ENABLED
                    , {tag, offset, bblIP}
#endif
                };
            //TODO: clean up
            uint64_t respCycle;
            if (withZeroAlloc) {
                respCycle = zeroAlloc(req);
            } else {
                respCycle = access(req);
            }


            if (withZeroAlloc && (zeroAllocLevel != 1)) {
                //end early it is zero alloc opt but NOT clzero1 because there is no line to replace on this level
            } else {
                //Due to the way we do the locking, at this point the old address might be invalidated, but we have the new address guaranteed until we release the lock

                //Careful with this order
                Address oldAddr = filterArray[idx].rdAddr;
                filterArray[idx].wrAddr = isLoad? -1L : vLineAddr;
                filterArray[idx].rdAddr = vLineAddr;

#ifdef CLU_STATS_ENABLED
                if (oldAddr != UNDEF_CACHE_LINE_ADDRESS) {
                    fCLEI++;
                    fUCLC += __builtin_popcount(filterArray[idx].accessMask);
                    filterArray[idx].accessMask = CLU_STATS_ZERO_MASK;
                }
                processAccessCLUStats(vAddr, size, memReqStatType, idx);
#endif

                //For LSU simulation purposes, loads bypass stores even to the same line if there is no conflict,
                //(e.g., st to x, ld from x+8) and we implement store-load forwarding at the core.
                //So if this is a load, it always sets availCycle; if it is a store hit, it doesn't
                if (oldAddr != vLineAddr) filterArray[idx].availCycle = respCycle;

            }

            futex_unlock(&filterLock);
            return respCycle + tlbLat;
        }

        uint64_t invalidate(const InvReq& req) {
            Cache::startInvalidate();  // grabs cache's downLock
            futex_lock(&filterLock);
            uint32_t idx = req.lineAddr & setMask; //works because of how virtual<->physical is done...
            if ((filterArray[idx].rdAddr | procMask) == req.lineAddr) { //FIXME: If another process calls invalidate(), procMask will not match even though we may be doing a capacity-induced invalidation!
#ifdef CLU_STATS_ENABLED
                assert(filterArray[idx].rdAddr != UNDEF_CACHE_LINE_ADDRESS);
#endif
                filterArray[idx].wrAddr = -1L;
                filterArray[idx].rdAddr = -1L;
#ifdef CLU_STATS_ENABLED
                fCLEI++;
                fUCLC += __builtin_popcount(filterArray[idx].accessMask);
                filterArray[idx].accessMask = CLU_STATS_ZERO_MASK;
#endif
            }
            uint64_t respCycle = Cache::finishInvalidate(req); // releases cache's downLock
            futex_unlock(&filterLock);
            return respCycle;
        }

        void contextSwitch() {
            futex_lock(&filterLock);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            futex_unlock(&filterLock);
        }

        //for zero allocation optimization
        inline uint64_t zeroAllocStore(Address vAddr, uint64_t curCycle
#ifdef CLU_STATS_ENABLED
                             , MASize_t size, MemReqStatType_t memReqStatType
#endif
#ifdef MA_PROF_ENABLED
                             , PointerTag_t tag, MAOffset_t offset, Address bblIP
#endif
                             , uint32_t tid, uint32_t zeroAllocLevel
                             ) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint32_t bytes = vAddr & ((1 << lineBits) - 1);
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            if ((zeroAllocLevel == 1) && (vLineAddr == filterArray[idx].wrAddr)) {
#ifdef CLU_STATS_ENABLED
                processAccessCLUStats(vAddr, size, StoreData, idx);
                uint32_t doubleWordIdx = bytes >> (uint32_t) log2(sizeof(uint64_t));
                //assert(doubleWordIdx < 16);
                assert(doubleWordIdx < 32);
                filterArray[idx].data[doubleWordIdx] = 0; //value;
#endif
                fGETXHit++;
                //NOTE: Stores don't modify availCycle; we'll catch matches in the core
                //filterArray[idx].availCycle = curCycle; //do optimistic store-load forwarding

                return MAX(curCycle, availCycle);
            } else {
                return replace(vLineAddr, idx, false, curCycle, vAddr
#ifdef CLU_STATS_ENABLED
                               , size, memReqStatType
#endif
#ifdef MA_PROF_ENABLED
                               , tag, offset, bblIP
#endif
                               , bytes, tid, true/*withZeroAlloc*/, zeroAllocLevel
                               );

            }
        }

        inline uint64_t cacheScrubInvalidate(Address vAddr, uint64_t curCycle
#ifdef CLU_STATS_ENABLED
                                            , MASize_t size, MemReqStatType_t memReqStatType
#endif
#ifdef MA_PROF_ENABLED
                                            , PointerTag_t tag, MAOffset_t offset, Address bblIP
#endif
                                            , uint32_t tid, uint32_t invalidateLevel
                                            ) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint32_t bytes = vAddr & ((1 << lineBits) - 1);
            //uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            uint64_t respCycle;
            // this line may or may not be present in the L1 cache.
            // If it is, scrubInvalidate will evict it.
            // If not, scrubInvalidate will return
            Address pLineAddr = procMask | vLineAddr;
            MESIState dummyState = MESIState::I;

            uint32_t localFlags = reqFlags;

            if (invalidateLevel == 1) {
                localFlags |= MemReq::CACHE_SCRUB_1;
            } else if (invalidateLevel == 2) {
                localFlags |= MemReq::CACHE_SCRUB_2;
            } else if (invalidateLevel == 3) {
                localFlags |= MemReq::CACHE_SCRUB_3;
            } else {
                assert(false);
            }

            futex_lock(&filterLock);
            MemReq req = {pLineAddr, GETS, 0 /*childId is hard-coded here*/, &dummyState, curCycle, &filterLock, dummyState, srcId,
                           (bytes << (MAX_MEMREQ_FLAGS + 1)) | localFlags
                              //{pLineAddr, isLoad? GETS : GETX, 0, &dummyState, curCycle, &filterLock, dummyState, srcId,  reqFlags | (bytes << 6)
#ifdef CLU_STATS_ENABLED
                          , {vAddr, size, memReqStatType, filterArray[idx].rdAddr, filterArray[idx].accessMask}
#endif
#ifdef MA_PROF_ENABLED
                          , {tag, offset, bblIP}
#endif
            };
            respCycle  = scrubInvalidate(req);
            assert(respCycle == curCycle || respCycle == curCycle + invLat || respCycle <= curCycle + 30);

            // if this line is in the filter cache, evict it
            if (invalidateLevel == 1) {
                if ((filterArray[idx].rdAddr | procMask) == req.lineAddr) { //FIXME: If another process calls invalidate(), procMask will not match even though we may be doing a capacity-induced invalidation!
                    assert(respCycle == curCycle + invLat);
#ifdef CLU_STATS_ENABLED
                    assert(filterArray[idx].rdAddr != UNDEF_CACHE_LINE_ADDRESS);
#endif
                    filterArray[idx].wrAddr = -1L;
                    filterArray[idx].rdAddr = -1L;
#ifdef CLU_STATS_ENABLED
                    fCLEI++;
                    fUCLC += __builtin_popcount(filterArray[idx].accessMask);
                    filterArray[idx].accessMask = CLU_STATS_ZERO_MASK;
#endif
                }
            }
            futex_unlock(&filterLock);
            return respCycle;
        }
};

#endif  // FILTER_CACHE_H_
