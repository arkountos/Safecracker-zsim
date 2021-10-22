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

#include "coherence_ctrls.h"
#include "cache.h"
#include "network.h"
#include "clu_stats.h"
#include "ma_prof.h"
#include "zsim.h"
#include "prefetcher.h"

#ifdef MAXSIM_ENABLED
#include "maxsim/maxsim_profiling.h"
#endif // MAXSIM_ENABLED

/* Do a simple XOR block hash on address to determine its bank. Hacky for now,
 * should probably have a class that deals with this with a real hash function
 * (TODO)
 */
uint32_t MESIBottomCC::getParentId(Address lineAddr) {
    //Hash things a bit
    uint32_t res = 0;
    uint64_t tmp = lineAddr;
    for (uint32_t i = 0; i < 4; i++) {
        res ^= (uint32_t) ( ((uint64_t)0xffff) & tmp);
        tmp = tmp >> 16;
    }
    return (res % parents.size());
}

#ifdef MA_PROF_ENABLED
void MESIBottomCC::initMAProf(MAProfCacheGroupId_t _MAProfCacheGroupId) {
    MAProfCacheGroupId = _MAProfCacheGroupId;
}
#endif

void MESIBottomCC::init(const g_vector<MemObject*>& _parents, Network* network, const char* name
#ifdef MA_PROF_ENABLED
                        , MAProfCacheGroupId_t _MAProfCacheGroupId
#endif
                        ) {
    parents.resize(_parents.size());
    parentRTTs.resize(_parents.size());
    for (uint32_t p = 0; p < parents.size(); p++) {
        parents[p] = _parents[p];
        parentRTTs[p] = (network)? network->getRTT(name, parents[p]->getName()) : 0;
    }
#ifdef MA_PROF_ENABLED
    initMAProf(_MAProfCacheGroupId);
#endif
}

void MESIBottomCC::resetParent(const g_vector<MemObject*>& _parents) {
    parents.resize(_parents.size());
    for (uint32_t p = 0; p < parents.size(); p++) {
        parents[p] = _parents[p];
    }
}

uint64_t MESIBottomCC::sendScrubInvReqToParent(MemReq& req) {
    uint32_t parentId = getParentId(req.lineAddr);
    Cache* parentCache = dynamic_cast<Cache*>(parents[parentId]);
    StreamPrefetcher* parentPrefetcher = dynamic_cast<StreamPrefetcher*>(parents[parentId]);
    if (parentCache) {
        return parentCache->scrubInvalidate(req);
    } else if (parentPrefetcher) {
        return parentPrefetcher->passScrubInvalidate(req);
    } else {
        assert_msg(false, "Currently only support Cache for scrub invalidation req OR there are fewer levels of caches than the requested optimization level");
    }
    return -1; // to make MaxSim build script happy
}

uint64_t MESIBottomCC::sendZeroAllocReqToParent(MemReq& req) {
    uint32_t parentId = getParentId(req.lineAddr);
    Cache* parentCache = dynamic_cast<Cache*>(parents[parentId]);
    StreamPrefetcher* parentPrefetcher = dynamic_cast<StreamPrefetcher*>(parents[parentId]);
    if (parentCache) {
        return parentCache->zeroAlloc(req);
    } else if (parentPrefetcher) {
        return parentPrefetcher->passZeroAlloc(req);
    } else {
        assert_msg(false, "Currently only support Cache for zero alloc req OR there are fewer levels of caches than the requested optimization level");
    }
    return -1; // to make MaxSim build script happy
}

uint64_t MESIBottomCC::processEviction(Address wbLineAddr, uint32_t lineId, bool lowerLevelWriteback, uint64_t cycle, const MemReq& triggerReq) {
    uint32_t srcId = triggerReq.srcId;
    MESIState* state = &array[lineId];
    // no flags unless it was cachescrub (CACHE_SCRUB), currently not for ZERO_ALLOC_1
    uint32_t flags = 0;
    if (lowerLevelWriteback) {
        //If this happens, when tcc issued the invalidations, it got a writeback. This means we have to do a PUTX, i.e. we have to transition to M if we are in E
        assert(*state == M || *state == E); //Must have exclusive permission!
        *state = M; //Silent E->M transition (at eviction); now we'll do a PUTX

        //should never hit here during CLINV
    }

    if (triggerReq.flags & MemReq::CACHE_SCRUB_1){
        assert(!lowerLevelWriteback);
        if (*state == M){
            // marking it clean to avoid writeback for clinv
            *state = E;
        }
        flags |= MemReq::CACHE_SCRUB_1;
    } else {
        assert(flags == 0);
    }

    uint64_t respCycle = cycle;
    switch (*state) {
        case I:
            break; //Nothing to do
        case S:
        case E:
            {
                MemReq req = {wbLineAddr, PUTS, selfId, state, cycle, &ccLock, *state, srcId, flags //0 /*no flags*/
#ifdef CLU_STATS_ENABLED
                        , {UNDEF_VIRTUAL_ADDRESS, UNDEF_MA_SIZE, MAUndefined, triggerReq.CLUStatsAttrs.replacedLineAddr, triggerReq.CLUStatsAttrs.replacedLineAccessMask}
#endif
#ifdef MA_PROF_ENABLED
                        , {UNDEF_TAG, UNDEF_OFFSET, UNDEF_VIRTUAL_ADDRESS}
#endif
                    };
#ifdef CLU_STATS_ENABLED
                profCLE.inc();
#endif
                respCycle = parents[getParentId(wbLineAddr)]->access(req);
            }
            break;
        case M:
            {
                MemReq req = {wbLineAddr, PUTX, selfId, state, cycle, &ccLock, *state, srcId, 0 /*no flags*/
#ifdef CLU_STATS_ENABLED
                        , {UNDEF_VIRTUAL_ADDRESS, UNDEF_MA_SIZE, MAUndefined, triggerReq.CLUStatsAttrs.replacedLineAddr, triggerReq.CLUStatsAttrs.replacedLineAccessMask}
#endif
#ifdef MA_PROF_ENABLED
                        , {UNDEF_TAG, UNDEF_OFFSET, UNDEF_VIRTUAL_ADDRESS}
#endif
                    };
#ifdef CLU_STATS_ENABLED
                profCLE.inc();
#endif
                respCycle = parents[getParentId(wbLineAddr)]->access(req);
            }
            break;

        default: panic("!?");
    }
    assert_msg(*state == I, "Wrong final state %s on eviction", MESIStateName(*state));
    return respCycle;
}

uint64_t MESIBottomCC::processAccess(Address lineAddr, uint32_t lineId, AccessType type, uint64_t cycle, uint32_t srcId, uint32_t flags
#ifdef CLU_STATS_ENABLED
                                     , Address virtualAddr, MASize_t memoryAccessSize, MemReqStatType_t memReqStatType
#endif
#ifdef MA_PROF_ENABLED
                                     , PointerTag_t tag, MAOffset_t offset, Address bblIP
#endif
                                     ) {
    uint64_t respCycle = cycle;
    MESIState* state = &array[lineId];
    if (flags & MemReq::CACHE_SCRUB_1) {
        assert(type == PUTS);
    }

    if (flags & MemReq::ZERO_ALLOC_1) {
        flags = downgradeZeroAllocFlag(flags);
    }
    switch (type) {
        // A PUTS/PUTX does nothing w.r.t. higher coherence levels --- it dies here
        case PUTS: //Clean writeback, nothing to do (except profiling)
            assert(*state != I);
            profPUTS.inc();
            break;
        case PUTX: //Dirty writeback
            assert(*state == M || *state == E);
            if (*state == E) {
                //Silent transition, record that block was written to
                *state = M;
            }
            profPUTX.inc();
            break;
        case GETS:
            if (*state == I) {
                uint32_t parentId = getParentId(lineAddr);
                MemReq req = {lineAddr, GETS, selfId, state, cycle, &ccLock, *state, srcId, flags
#ifdef CLU_STATS_ENABLED
                        , {virtualAddr, memoryAccessSize, memReqStatType, UNDEF_CACHE_LINE_ADDRESS, CLU_STATS_ZERO_MASK}
#endif
#ifdef MA_PROF_ENABLED
                        , {tag, offset, bblIP}
#endif
                    };
                MemReq tmpReq = req; 

                uint32_t nextLevelLat = parents[parentId]->access(req) - cycle;
                uint32_t netLat = parentRTTs[parentId];
                profGETNextLevelLat.inc(nextLevelLat);
                profGETNetLat.inc(netLat);
                respCycle += nextLevelLat + netLat;
#ifdef MA_PROF_ENABLED
#   ifdef MAXSIM_ENABLED
                //MaxSimProfiling::getInst().addCacheMiss(tag, offset, bblIP, false, MAProfCacheGroupId, 1);
#   else
                UNUSED_VAR(tag); UNUSED_VAR(offset); UNUSED_VAR(bblIP);
#   endif
#endif
                profGETSMiss.inc();
                //TODO we have to fix this... I saw significant amount of re-issues in some benchmarks such as 104.svm
                if (!(*state == S || *state == E)) {
                    info("Prefetchers doing wrong thing... Re-issue request");
                    parents[parentId]->access(tmpReq);
                }

                assert_msg(*state == S || *state == E,
                        "parents %s state %s srcId %d",
                        parents[parentId]->getName(),
                        MESIStateName(*state),
                        srcId);
            } else {
                profGETSHit.inc();
            }
            break;
        case GETX:
            if (*state == I || *state == S) {
#ifdef MA_PROF_ENABLED
#   ifdef MAXSIM_ENABLED
                //MaxSimProfiling::getInst().addCacheMiss(tag, offset, bblIP, true, MAProfCacheGroupId, 1);
#   else
                UNUSED_VAR(tag); UNUSED_VAR(offset); UNUSED_VAR(bblIP);
#   endif
#endif
                if (flags & MemReq::ZERO_ALLOC) {
                    if (*state == I) profClZeroGETXMissIM.inc();
                    else profClZeroGETXMissSM.inc();
                }  else {
                    //Profile before access, state changes
                    if (*state == I) profGETXMissIM.inc();
                    else profGETXMissSM.inc();
                }
                uint32_t parentId = getParentId(lineAddr);
                MemReq req = {lineAddr, GETX, selfId, state, cycle, &ccLock, *state, srcId, flags
#ifdef CLU_STATS_ENABLED
                        , {virtualAddr, memoryAccessSize, memReqStatType, UNDEF_CACHE_LINE_ADDRESS, CLU_STATS_ZERO_MASK}
#endif
#ifdef MA_PROF_ENABLED
                        , {tag, offset, bblIP}
#endif
                    };
                uint32_t nextLevelLat = parents[parentId]->access(req) - cycle;
                uint32_t netLat = parentRTTs[parentId];
                if (flags & MemReq::ZERO_ALLOC) {
                    // no change to respCycle because we assume that zero alloc only has lat for first level
                } else {
                    profGETNextLevelLat.inc(nextLevelLat);
                    profGETNetLat.inc(netLat);
                    respCycle += nextLevelLat + netLat;
                }
            } else {
                if (*state == E) {
                    // Silent transition
                    // NOTE: When do we silent-transition E->M on an ML hierarchy... on a GETX, or on a PUTX?
                    /* Actually, on both: on a GETX b/c line's going to be modified anyway, and must do it if it is the L1 (it's OK not
                     * to transition if L2+, we'll TX on the PUTX or invalidate, but doing it this way minimizes the differences between
                     * L1 and L2+ controllers); and on a PUTX, because receiving a PUTX while we're in E indicates the child did a silent
                     * transition and now that it is evictiong, it's our turn to maintain M info.
                     */
                    *state = M;
                }

                if (flags & MemReq::ZERO_ALLOC) {
                    profClZeroGETXHit.inc();
                } else {
                    profGETXHit.inc();
                }
            }
            assert_msg(*state == M, "Wrong final state on GETX, lineId %d numLines %d, finalState %s", lineId, numLines, MESIStateName(*state));
            break;

        default: panic("!?");
    }
    assert_msg(respCycle >= cycle, "XXX %ld %ld", respCycle, cycle);
    return respCycle;
}

void MESIBottomCC::processWritebackOnAccess(Address lineAddr, uint32_t lineId, AccessType type) {
    MESIState* state = &array[lineId];
    assert(*state == M || *state == E);
    if (*state == E) {
        //Silent transition to M if in E
        *state = M;
    }
}

void MESIBottomCC::processInval(Address lineAddr, uint32_t lineId, InvType type, bool* reqWriteback) {
    if (IsCacheScrubbingInv(type) && (lineId == (unsigned) -1)){
        return;
    }
    MESIState* state = &array[lineId];
    assert(*state != I);
    switch (type) {
        case INVX: //lose exclusivity
            //Hmmm, do we have to propagate loss of exclusivity down the tree? (nah, topcc will do this automatically -- it knows the final state, always!)
            assert_msg(*state == E || *state == M, "Invalid state %s", MESIStateName(*state));
            if (*state == M) *reqWriteback = true;
            *state = S;
            profINVX.inc();
            break;
        case INV: //invalidate
            assert(*state != I);
            if (*state == M) *reqWriteback = true;
            *state = I;
            profINV.inc();
            break;
        case FWD: //forward
            assert_msg(*state == S, "Invalid state %s on FWD", MESIStateName(*state));
            profFWD.inc();
            break;
        case CLINV: //invalidate
            assert(*state != I); //            *reqWriteback = true;
            *state = I;
            profClInvINV.inc();
            break;
        default: panic("!?");
    }
    //NOTE: BottomCC never calls up on an invalidate, so it adds no extra latency
}


uint64_t MESIBottomCC::processNonInclusiveWriteback(Address lineAddr, AccessType type, uint64_t cycle, MESIState* state, uint32_t srcId, uint32_t flags) {
    if (!nonInclusiveHack) panic("Non-inclusive %s on line 0x%lx, this cache should be inclusive", AccessTypeName(type), lineAddr);

    //info("Non-inclusive wback, forwarding");
    MemReq req = {lineAddr, type, selfId, state, cycle, &ccLock, *state, srcId, flags | MemReq::NONINCLWB
#ifdef CLU_STATS_ENABLED
            , {UNDEF_VIRTUAL_ADDRESS, UNDEF_MA_SIZE, MAUndefined, UNDEF_CACHE_LINE_ADDRESS, CLU_STATS_ZERO_MASK}
#endif
#ifdef MA_PROF_ENABLED
            , {UNDEF_TAG, UNDEF_OFFSET, UNDEF_VIRTUAL_ADDRESS}
#endif
        };
    uint64_t respCycle = parents[getParentId(lineAddr)]->access(req);
    return respCycle;
}


/* MESITopCC implementation */

void MESITopCC::init(const g_vector<BaseCache*>& _children, Network* network, const char* name) {
    if (_children.size() > MAX_CACHE_CHILDREN) {
        panic("[%s] Children size (%d) > MAX_CACHE_CHILDREN (%d)", name, (uint32_t)_children.size(), MAX_CACHE_CHILDREN);
    }
    children.resize(_children.size());
    childrenRTTs.resize(_children.size());
    for (uint32_t c = 0; c < children.size(); c++) {
        children[c] = _children[c];
        childrenRTTs[c] = (network)? network->getRTT(name, children[c]->getName()) : 0;
    }
}

uint64_t MESITopCC::sendInvalidates(Address lineAddr, uint32_t lineId, InvType type, bool* reqWriteback, uint64_t cycle, uint32_t srcId) {
    //Send down downgrades/invalidates
    Entry* e = &array[lineId];

    //Don't propagate downgrades if sharers are not exclusive.
    if (type == INVX && !e->isExclusive()) {
        return cycle;
    }
    //info("  MESITopCC::sendinvalidates lineId %d", lineId);
    uint64_t maxCycle = cycle; //keep maximum cycle only, we assume all invals are sent in parallel
    if (!e->isEmpty()) {
        uint32_t numChildren = children.size();
        uint32_t sentInvs = 0;
        for (uint32_t c = 0; c < numChildren; c++) {
            //info("  MESITopCC::sendinvalidates to lineAddr: %lx c = %d isSharer %d", lineAddr << 6, c, e->sharers[c] == true);
            if (e->sharers[c]) {
                InvReq req = {lineAddr, type, reqWriteback, cycle, srcId};
                uint64_t respCycle = children[c]->invalidate(req);
                respCycle += childrenRTTs[c];
                maxCycle = MAX(respCycle, maxCycle);
                if (type == INV || IsCacheScrubbingInv(type)) e->sharers[c] = false;
                sentInvs++;
            }
        }
        assert_msg(sentInvs == e->numSharers, "lineId %x isLineId == -1 %u sentInvs %u numSharers %x type==cacheScrubbing %u", lineId, lineId == (unsigned)-1, sentInvs, e->numSharers, IsCacheScrubbingInv(type));
        if (type == INV) {
            e->numSharers = 0;
        } else if (IsCacheScrubbingInv(type)) {
            assert(!*reqWriteback);
            e->numSharers = 0;
        } else {
            //TODO: This is kludgy -- once the sharers format is more sophisticated, handle downgrades with a different codepath
            assert(e->exclusive);
            assert(e->numSharers == 1);
            e->exclusive = false;
        }
    }
    return maxCycle;
}

uint64_t MESITopCC::processEviction(Address wbLineAddr, uint32_t lineId, bool* reqWriteback, uint64_t cycle, uint32_t srcId) {
    if (nonInclusiveHack) {
        // Don't invalidate anything, just clear our entry
        array[lineId].clear();
        return cycle;
    } else {
        //Send down invalidates
        return sendInvalidates(wbLineAddr, lineId, INV, reqWriteback, cycle, srcId);
    }
}

uint64_t MESITopCC::processEvictionWithNoWriteBack(Address wbLineAddr, uint32_t lineId, bool* reqWriteback, uint64_t cycle, uint32_t srcId){
    if (nonInclusiveHack) {
        // Don't invalidate anything, just clear our entry
        array[lineId].clear();
        return cycle;
    } else {
        //Send down invalidates
        uint64_t respCycle =  sendInvalidates(wbLineAddr, lineId, CLINV, reqWriteback, cycle, srcId);
        assert(!*reqWriteback);
        return respCycle;
    }
}

uint64_t MESITopCC::processAccess(Address lineAddr, uint32_t lineId, AccessType type, uint32_t childId, bool haveExclusive,
                                  MESIState* childState, bool* inducedWriteback, uint64_t cycle, uint32_t srcId, uint32_t flags) {
    Entry* e = &array[lineId];
    uint64_t respCycle = cycle;
    switch (type) {
        case PUTX:
            assert(e->isExclusive());
            if (flags & MemReq::PUTX_KEEPEXCL) {
                assert(e->sharers[childId]);
                assert(*childState == M);
                *childState = E; //they don't hold dirty data anymore
                break; //don't remove from sharer set. It'll keep exclusive perms.
            }
            //note NO break in general
        case PUTS:
            //TODO: CACHE_SCRUB_INV should hit here: need to add separate profile
            assert(e->sharers[childId]);
            e->sharers[childId] = false;
            e->numSharers--;
            *childState = I;
            break;
        case GETS:
            if (e->isEmpty() && haveExclusive && !(flags & MemReq::NOEXCL)) {
                //Give in E state
                e->exclusive = true;
                e->sharers[childId] = true;
                e->numSharers = 1;
                *childState = E;
            } else {
                //Give in S state
                assert(e->sharers[childId] == false);

                if (e->isExclusive()) {
                    //Downgrade the exclusive sharer
                    respCycle = sendInvalidates(lineAddr, lineId, INVX, inducedWriteback, cycle, srcId);
                }

                assert_msg(!e->isExclusive(), "Can't have exclusivity here. isExcl=%d excl=%d numSharers=%d", e->isExclusive(), e->exclusive, e->numSharers);

                e->sharers[childId] = true;
                e->numSharers++;
                e->exclusive = false; //dsm: Must set, we're explicitly non-exclusive
                *childState = S;
            }
            break;
        case GETX:
            assert(haveExclusive); //the current cache better have exclusive access to this line

            if (flags & MemReq::ZERO_ALLOC_1) {
                e->exclusive = true;
                assert(e->numSharers <= 1);
            } else {
                // If child is in sharers list (this is an upgrade miss), take it out
                if (e->sharers[childId]) {
                    assert_msg(!e->isExclusive(), "Spurious GETX, childId=%d numSharers=%d isExcl=%d excl=%d", childId, e->numSharers, e->isExclusive(), e->exclusive);
                    e->sharers[childId] = false;
                    e->numSharers--;
                }

                // Invalidate all other copies
                respCycle = sendInvalidates(lineAddr, lineId, INV, inducedWriteback, cycle, srcId);

                // Set current sharer, mark exclusive
                e->sharers[childId] = true;
                e->numSharers++;
                e->exclusive = true;

                assert(e->numSharers == 1);
            }

            *childState = M; //give in M directly
            break;

        default: panic("!?");
    }

    return respCycle;
}

uint64_t MESITopCC::processInval(Address lineAddr, uint32_t lineId, InvType type, bool* reqWriteback, uint64_t cycle, uint32_t srcId) {
    if (type == FWD) {//if it's a FWD, we should be inclusive for now, so we must have the line, just invLat works
        assert(!nonInclusiveHack); //dsm: ask me if you see this failing and don't know why
        return cycle;
    } else {
        //Just invalidate or downgrade down to children as needed
        return sendInvalidates(lineAddr, lineId, type, reqWriteback, cycle, srcId);
    }
}

/*MESIDirCC*/

MESIDirCC::MESIDirCC(uint32_t _numLines, const g_string& _name) : numLines(_numLines), name(_name) {
    info("%s is a Dir with lines %d", name.c_str(), numLines);
    array = gm_calloc<Tag>(numLines);
    for (uint32_t i = 0; i < numLines; i++) {
        new (&array[i]) Tag();
    }

    futex_init(&upLock);
    futex_init(&downLock);
}

void MESIDirCC::setParents(uint32_t childId, const g_vector<MemObject*>& _parents, Network* network) {
    selfId = childId;
    parents.resize(_parents.size());
    parentRTTs.resize(_parents.size());
    for (uint32_t p = 0; p < parents.size(); p++) {
        parents[p] = _parents[p];
        parentRTTs[p] = (network)? network->getRTT(name.c_str(), parents[p]->getName()) : 0;
    }
}

void MESIDirCC::setChildren(const g_vector<BaseCache*>& _children, Network* network) {
    if (_children.size() > MAX_CACHE_CHILDREN) {
        panic("[%s] Children size (%d) > MAX_CACHE_CHILDREN (%d)", name.c_str(), (uint32_t)_children.size(), MAX_CACHE_CHILDREN);
    }
    children.resize(_children.size());
    childrenRTTs.resize(_children.size());
    for (uint32_t c = 0; c < children.size(); c++) {
        children[c] = _children[c];
        childrenRTTs[c] = (network)? network->getRTT(name.c_str(), children[c]->getName()) : 0;
    }
}

uint32_t MESIDirCC::getNumChildren() const {
    return children.size();
}

const BaseCache* MESIDirCC::getChild(uint32_t child) const {
    assert(child < getNumChildren());
    return children[child];
}

void MESIDirCC::initStats(AggregateStat* cacheStat) {
    auto initCounter = [cacheStat] (Counter& c, const char* shortDesc, const char* longDesc) {
        c.init(shortDesc, longDesc);
        cacheStat->append(&c);
    };

    //Request stats
    initCounter(profGETSChild,  "cGETS", "GETS satisfied from child");
    initCounter(profGETSParent, "pGETS", "GETS satisfied from parent");
    initCounter(profGETXChild,  "cGETX", "GETX satisfied from child");
    initCounter(profGETXParent, "pGETX", "GETX satisfied from parent");

    initCounter(profPUTSFiltered, "cPUTS", "PUTS from lower level, filtered");
    initCounter(profPUTSParent, "pPUTS", "PUTX from lower level, passed to parent");
    initCounter(profPUTX, "PUTX", "PUTX from lower level (always passed to parent)");

    initCounter(profINV, "INV", "INV from parent");
    initCounter(profINVX, "INVX", "INVX from parent");
    initCounter(profFWD, "FWD", "FWD from parent");


    //Eviction stats
    initCounter(profEvUNUSED, "evUNUSED", "Evictions that find an unused line");
    initCounter(profEvUSED, "evUSED", "Evictions on used lines");
    initCounter(profEvINV, "evINV", "Invalidates sent due to evictions");

    //Writeback stats
    initCounter(profWbINVX, "wbINVX", "Dirty writebacks on exclusive downgrades (to parent)");
    initCounter(profWbEvINV, "wbEvINV", "Dirty writebacks on eviction-initiated invalidates (to parent)");
    //NOTE: coherence-initiated writebacks on GETX are avoided, as we pass the line in M to the new sharer

    //Latency stats
    initCounter(profGETNextLevelLat, "latGETnl", "GET request latency on next level");
    initCounter(profGETNetLat, "latGETnet", "GET request latency on network to next level");
    initCounter(profINVCPLat, "latINVget", "Critical-path latency on GET-initiated INV requests");
    initCounter(profINVXCPLat, "latINVXget", "Critical-path latency on GET-initiated INVX requests");
    initCounter(profFWDCPLat, "latFWDget", "Critical-path latency on GET-initiated FWD requests");
}

//Access methods
bool MESIDirCC::startAccess(MemReq& req) {
    assert((req.type == GETS) || (req.type == GETX) || (req.type == PUTS) || (req.type == PUTX));

    /* Child should be locked when called. We do hand-over-hand locking when going
     * down (which is why we require the lock), but not when going up, opening the
     * child to invalidation races here to avoid deadlocks.
     */
    if (req.childLock) {
        futex_unlock(req.childLock);
    }

    futex_lock(&upLock); //must lock up first to avoid deadlock
    futex_lock(&downLock);

    /* The situation is now stable, true race-wise. No one can touch the child state, because we hold
     * both parent's locks. So, we first handle races, which may cause us to skip the access.
     */
    bool skipAccess = CheckForMESIRace(req.type /*may change*/, req.state, req.initialState);
    return skipAccess;
}

bool MESIDirCC::shouldAllocate(const MemReq& req) {
    assert((req.type == GETS) || (req.type == GETX)); //dir is inclusive, PUTS/X have to hit
    return true;
}

uint64_t MESIDirCC::processEviction(const MemReq& triggerReq, Address wbLineAddr, int32_t lineId, uint64_t startCycle) {
    if (array[lineId].state == I) {
        profEvUNUSED.inc();
        return startCycle; //skip invalidates
    }
    //line is S or E
    profEvUSED.inc();
    profEvINV.inc(array[lineId].numSharers);
    bool reqWriteback = false;
    uint64_t respCycle = sendInvalidates(wbLineAddr, lineId, INV, &reqWriteback, startCycle, triggerReq.srcId);
    if (reqWriteback) { //not in critical path
        assert(array[lineId].state == E);
        //info("%s Process eviction (PUTX) wbLineAddr %lu because of req.lineAddr %lu, req.srcId %d", getName(), wbLineAddr, triggerReq.lineAddr, triggerReq.srcId);
        issueParentAccess(wbLineAddr, lineId, PUTX, respCycle, triggerReq.srcId, 0);
        profWbEvINV.inc();
    } else {
        //info("%s Process eviction (PUTS) wbLineAddr %lu because of req.lineAddr %lu, req.srcId %d", getName(), wbLineAddr, triggerReq.lineAddr, triggerReq.srcId);
        issueParentAccess(wbLineAddr, lineId, PUTS, respCycle, triggerReq.srcId, 0);
    }
    assert(array[lineId].state == I);
    return respCycle;
}

uint64_t MESIDirCC::processAccess(const MemReq& req, int32_t lineId, uint64_t startCycle, uint64_t* getDoneCycle) {
    assert(lineId != -1); //inclusive, all accesses allocate
    assert(!getDoneCycle); //only TimingCache needs this, TODO: implement when needed
    //info("%s Process access: req.lineAddr %lu, req.srcId %d req.type %s req is NOEXL %d startCycle: %lu", getName(), req.lineAddr, req.srcId, AccessTypeName(req.type), req.is(MemReq::NOEXCL), startCycle);

    Tag& tag = array[lineId];
    MESIState& state = tag.state;
    uint64_t respCycle = startCycle;
    switch (req.type) {
        case GETS:
            {
                bool giveExcl = true;
                if (state == I) {
                    uint32_t netLat = 0;
                    //info("%s issue Parent Access (GETS) to get Data: req.lineAddr: %lu, startCycle %lu, req.srcId %d, req.type %s", getName(), req.lineAddr, startCycle, req.srcId, AccessTypeName(req.type));
                    respCycle = issueParentAccess(req.lineAddr, lineId, GETS, startCycle, req.srcId, req.flags, &netLat);
                    profGETSParent.inc();
                    profGETNetLat.inc(netLat);
                    profGETNextLevelLat.inc(respCycle - startCycle - netLat);
                    giveExcl = (state == E); //if S, can't give EXCL
                } else {
                    assert(state == S || state == E);
                    if (tag.hasExclSharer()) {
                        bool reqWriteback = false;
                        respCycle = sendInvalidates(req.lineAddr, lineId, INVX, &reqWriteback, startCycle, req.srcId);
                        if (reqWriteback) {
                            state = M; //temporarily
                            //info("%s issue Parent Access (PUTX) to writeBack: req.lineAddr: %lu, respCycle %lu, req.srcId %d", getName(), req.lineAddr, respCycle, req.srcId);
                            issueParentAccess(req.lineAddr, lineId, PUTX, respCycle, req.srcId, MemReq::PUTX_KEEPEXCL); //flag maintains us as exclusive sharer

                            // Deal with PUTX_KEEPEXCL race: If an invalidate raced with us, we do not retain exclusivity, are turned into a PUTS, and get back I
                            // If this is the case, we assume the invalidate happened fully before the access started, and restart the access
                            if (state != E) {
                                assert(state == I);
                                assert(tag.sharers == 0);
                                //info("PUTX_KEEPEXCL race");
                                //OK to do because we have not touched any profiling counters and the tag is in a stable state. But we might want to rearchitect the code to be able to rewind
                                //Also, note that because we're in I, infinite resursion/livelock is impossible; we follow the I GETS path, which does not recurse
                                return processAccess(req, lineId, startCycle);
                            }
                            profWbINVX.inc();
                        }
                        profINVXCPLat.inc(respCycle-startCycle);
                    } else {
                        //dsm: OMG THE FUTURE IS NOW AND THE DEADLINE IS IN 2 DAYS
                        //Need to get a child to FWD us the data (TODO: in the future, can config to check upper level speculatively/also/instead)
                        //respCycle = sendFwd(req.lineAddr, lineId, startCycle, req.srcId);
                        //profFWDCPLat.inc(respCycle-startCycle);

                        uint32_t netLat = 0;
                        respCycle = issueParentAccess(req.lineAddr, lineId, GETS, startCycle, req.srcId, req.flags, &netLat);
                        profGETSParent.inc();
                        profGETNetLat.inc(netLat);
                        profGETNextLevelLat.inc(respCycle - startCycle - netLat);
                    }
                    giveExcl = false;
                    profGETSChild.inc();
                }
                assert(state == S || state == E);
                if (req.is(MemReq::NOEXCL)) giveExcl = false;
                tag.addSharer(req.childId, giveExcl);
                *req.state = giveExcl? E : S;
            }
            break;
        case GETX:
            {
                uint32_t upperNetLat = 0;
                uint64_t upperRespCycle = startCycle;
                if (state != E) {
                    upperRespCycle = issueParentAccess(req.lineAddr, lineId, GETX, startCycle, req.srcId, req.flags, &upperNetLat);
                    assert(state == M);
                    state = E; //we do not have data, so no M; lower levels will issue a PUTX
                }

                //NOTE: Race-wise, if state != E and we did the access, an intervening invalidate may have blown up all our sharer set
                //(only INV is possible, not INVX). That's OK, we still get E and can give out line in M.

                //On an upgrade miss, avoid sending child an INV!
                if (tag.isSharer(req.childId)) tag.removeSharer(req.childId);

                bool reqWriteback = false;
                uint64_t lowerRespCycle = sendInvalidates(req.lineAddr, lineId, INV, &reqWriteback, startCycle, req.srcId);
                //reqWriteback may be pulled up to true; even in that case, we don't need to WB to parent, because we pass the dirty line (in M) to the requesting child.

                //Both reqs can proceed in parallel; max determines critical path
                respCycle = MAX(upperRespCycle, lowerRespCycle);
                if (upperRespCycle > lowerRespCycle) { //ties broken in favor of lower
                    assert(upperRespCycle > startCycle); //there needs to be SOME delay... this may only fail if parent has a latency of 0
                    profGETXParent.inc();
                    profGETNetLat.inc(upperNetLat);
                    profGETNextLevelLat.inc(upperRespCycle - startCycle - upperNetLat);
                } else {
                    assert(lowerRespCycle >= startCycle); //covers the edge case of an S->M upgrade when the dir already has E, which causes no external traffic and no delays
                    profGETXChild.inc();
                    profINVCPLat.inc(lowerRespCycle - startCycle);
                }

                assert(!req.is(MemReq::NOEXCL)); //a NOEXCL GETX? Hmmm :)
                tag.addSharer(req.childId, true /*exclusive*/);
                *req.state = M; //give in M
            }
            break;
        case PUTS:
        case PUTX:
            {
                if (req.type == PUTX && req.is(MemReq::PUTX_KEEPEXCL)) {
                    //This is subject to races, as parent may turn us into a PUTS and get back I
                    assert(*req.state == M);
                    state = M; //temporary
                    respCycle = issueParentAccess(req.lineAddr, lineId, PUTX, respCycle, req.srcId, MemReq::PUTX_KEEPEXCL);
                    if (state == E) { //no race
                        assert(*req.state == M);
                        *req.state = E;
                    } else { //race, we lost all privileges, it's OK, child will do whatever necessary to get adequate permissions
                        assert(state == I); //our PUT invalidated us
                        assert(*req.state == I || *req.state == S); //if racing INV, child got I; with INVX, child got S
                        if (*req.state == S) {
                            tag.removeSharer(req.childId);
                            *req.state = I;
                        }
                    }
                    profPUTX.inc();
                } else {
                    if (req.type == PUTS) {
                        if (tag.numSharers > 1) profPUTSFiltered.inc();
                        else profPUTSParent.inc();
                    } else {
                        profPUTX.inc();
                    }

                    if (tag.numSharers == 1) { //last sharer
                        respCycle = issueParentAccess(req.lineAddr, lineId, req.type, respCycle, req.srcId, 0);
                        assert(state == I);
                    }
                    if (*req.state != I) { //racing invalidate could have already switched us to I
                        tag.removeSharer(req.childId);
                        *req.state = I;
                    }
                }
            }
            break;
        default: panic("?");
    }

    tag.check();


    //info("DIR %ld %ld", startCycle, respCycle);
    return respCycle;
}

void MESIDirCC::endAccess(const MemReq& req) {
    //Relock child before we unlock ourselves (hand-over-hand)
    if (req.childLock) {
        futex_lock(req.childLock);
    }

    futex_unlock(&downLock);
    futex_unlock(&upLock);
}

//Inv methods
void MESIDirCC::startInv() {
    futex_lock(&downLock);
}

void MESIDirCC::releaseInvLock() {
    futex_unlock(&downLock);
}

uint64_t MESIDirCC::processInv(const InvReq& req, int32_t lineId, uint64_t startCycle) {
    Address lineAddr = req.lineAddr;
    InvType type = req.type;
    bool* reqWriteback = req.writeback;
    uint32_t srcId = req.srcId;
    uint64_t respCycle = startCycle;
    if (type == FWD) {
        if (array[lineId].numSharers != 0) {
            respCycle = sendFwd(lineAddr, lineId, startCycle, srcId);
        } else {
            // racing invalidate, so parent already got the data (only in 3+-level setups...
            assert(array[lineId].state != I);
        }
        profFWD.inc();
    } else {
        respCycle = sendInvalidates(lineAddr, lineId, type, reqWriteback, startCycle, srcId);
        //Adjust state
        MESIState& state = array[lineId].state;
        switch (type) {
            case INVX:
                assert(state == E || state == M); //M state can happen if we race with a dirty wback
                //assert(array[lineId].numSharers != 0); //can be true when an inv races with a wback
                assert(!array[lineId].hasExclSharer());
                state = S;
                profINVX.inc();
                break;
            case INV:
                assert(state == E || state == S || state == M); //M state can happen if we race with a dirty wback
                assert(array[lineId].numSharers == 0);
                state = I;
                profINV.inc();
                break;
            default: panic("?");
        }
    }

    //With FWD/INVX, may have inconsistent lines b/c of how we handle inv/fwd races (geez)
    if (type == INV) array[lineId].check();

    futex_unlock(&downLock);
    return respCycle;
}

//Repl policy interface
uint32_t MESIDirCC::numSharers(uint32_t lineId) {return array[lineId].numSharers;}
bool MESIDirCC::isValid(uint32_t lineId) {return getState(lineId) != I;}
MESIState MESIDirCC::getState(uint32_t lineId) const {assert(lineId < numLines); return array[lineId].state;}

//Misc

//FIXME: Same as bcc; should be different or use same code...
uint32_t MESIDirCC::getParentId(Address lineAddr) const {
    //Hash things a bit
    uint32_t res = 0;
    uint64_t tmp = lineAddr;
    for (uint32_t i = 0; i < 4; i++) {
        res ^= (uint32_t) ( ((uint64_t)0xffff) & tmp);
        tmp = tmp >> 16;
    }
    return (res % parents.size());
}

//Internal functions
uint64_t MESIDirCC::sendInvalidates(Address lineAddr, uint32_t lineId, InvType type, bool* reqWriteback, uint64_t cycle, uint32_t srcId) {
    Tag& tag = array[lineId];
    uint64_t respCycle = cycle;
    if (type == INVX) {
        if (tag.hasExclSharer()) {
            assert_msg(tag.hasExclSharer(), "Invalid tag s: %s es: %d ns: %d", MESIStateName(tag.state), tag.exclSharer, tag.numSharers);
            //info("%s INVX req.lineAddr: %lu, respCycle %lu, req.srcId %d", getName(), lineAddr, cycle, srcId);
            uint32_t c = tag.getExclSharer();
            InvReq req = {lineAddr, INVX, reqWriteback, cycle, srcId};
            respCycle = children[c]->invalidate(req);
            respCycle += childrenRTTs[c];
            tag.removeSharer(c);
            tag.addSharer(c, false /*non-exclusive*/);
            assert(!tag.hasExclSharer());
        } //else nothing to do
    } else {
        assert(type == INV);
        uint32_t numChildren = children.size();
        for (uint32_t c = 0; c < numChildren; c++) {
            if (tag.isSharer(c)) {
                InvReq req = {lineAddr, INV, reqWriteback, cycle, srcId};
                uint64_t rCycle = children[c]->invalidate(req);
                rCycle += childrenRTTs[c];
                respCycle = MAX(respCycle, rCycle);
                tag.removeSharer(c);
            }
        }
    }
    //info("SI %ld %ld %d", cycle, respCycle, type);
    return respCycle;
}

uint64_t MESIDirCC::sendFwd(Address lineAddr, uint32_t lineId, uint64_t cycle, uint32_t srcId) {
    Tag& tag = array[lineId];
    assert(tag.numSharers);

    // FIXME(dsm): We have disabled forwards for the Jenga deadline. As a sanity check, we panic.
    panic("A forward just happened");

    //info("FWD 0x%lx %s es: %d ns: %d", lineAddr, MESIStateName(tag.state), tag.exclSharer, tag.numSharers);

    //Find closest sharer (in practice, this is a simple function when the topology is known)
    uint32_t closestSharer = children.size();
    uint32_t minRTT = 1<<30;
    for (uint32_t c = 0; c < children.size(); c++) {
         if (tag.isSharer(c) && childrenRTTs[c] < minRTT) {
             closestSharer = c;
             minRTT = childrenRTTs[c];
         }
    }
    assert(closestSharer < children.size());

    //Send fwd
    bool reqWriteback = false;
    InvReq req = {lineAddr, FWD, &reqWriteback, cycle, srcId};
    uint64_t respCycle = children[closestSharer]->invalidate(req);
    assert(!reqWriteback);
    respCycle += minRTT;
    //info("FWD %ld %ld", cycle, respCycle);
    return respCycle;
}

uint64_t MESIDirCC::issueParentAccess(Address lineAddr, uint32_t lineId, AccessType type, uint64_t cycle, uint32_t srcId, uint32_t flags, uint32_t* outNetLat) {
    MESIState* state = &array[lineId].state;
    MemReq req = {lineAddr, type, selfId, state, cycle, &downLock, *state, srcId, flags};
    uint32_t parentId = getParentId(lineAddr);
    uint64_t respCycle = parents[parentId]->access(req);
    uint32_t netLat = parentRTTs[parentId];
    respCycle += netLat;
    if (outNetLat) *outNetLat = netLat;
    //info("IPA %ld %ld", cycle, respCycle);
    return respCycle;
}

//Tag consistency checks -- call only when stable, i.e., all manipulations done
void MESIDirCC::Tag::check() const {
    auto xassert = [this] (bool cond) {
        assert_msg(cond, "Inconsistent line state: %s es: %d ns: %d", MESIStateName(state), exclSharer, numSharers);
    };

    if (state == I) {
        xassert(numSharers == 0);
        xassert(exclSharer == -1);
    } else if (state == S) {
        xassert(numSharers);
        xassert(exclSharer == -1);
    } else {
        xassert(state == E); //no M
        xassert(numSharers);
        if (numSharers > 1) xassert(exclSharer == -1);
    }
}
