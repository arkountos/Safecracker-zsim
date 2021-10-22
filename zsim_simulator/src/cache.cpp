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

#include "cache.h"
#include "hash.h"

#include "event_recorder.h"
#include "timing_event.h"
#include "zsim.h"

#ifdef MAXSIM_ENABLED
#include "maxsim/maxsim_profiling.h"
#include "maxsim/maxsim_runtime_info.h"
#endif // MAXSIM_ENABLED

Cache::Cache(CC* _cc, CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, uint32_t _tagLat, const g_string& _name)
    : cc(_cc), array(_array), rp(_rp), accLat(_accLat), invLat(_invLat), tagLat(_tagLat), name(_name) {}

const char* Cache::getName() {
    return name.c_str();
}

void Cache::setParents(uint32_t childId, const g_vector<MemObject*>& parents, Network* network) {
    cc->setParents(childId, parents, network);
}

void Cache::resetParents(const g_vector<MemObject*>& parents) {
    cc->resetParents(parents);
}

void Cache::setChildren(const g_vector<BaseCache*>& children, Network* network) {
    cc->setChildren(children, network);
}

void Cache::initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name.c_str(), "Cache stats");
    profClInv.init("ClInv", "Actual # of lines invalidated by ClInv in target level");
    cacheStat->append(&profClInv);
    initCacheStats(cacheStat);
    parentStat->append(cacheStat);
}

void Cache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    array->initStats(cacheStat);
    rp->initStats(cacheStat);
#ifdef RANGE_ACCESSES_PROFILING_ENABLED
    profGETSByRanges.init("GETSByRanges", "Number of GETS to different ranges", 9);
    profGETXByRanges.init("GETXByRanges", "Number of GETX to different ranges", 9);
    profPUTSByRanges.init("PUTSByRanges", "Number of PUTS to different ranges", 9);
    profPUTXByRanges.init("PUTXByRanges", "Number of PUTX to different ranges", 9);
    cacheStat->append(&profGETSByRanges);
    cacheStat->append(&profGETXByRanges);
    cacheStat->append(&profPUTSByRanges);
    cacheStat->append(&profPUTXByRanges);
#endif
}

uint64_t Cache::zeroAlloc(MemReq& req) {
    if (req.flags & MemReq::ZERO_ALLOC_1) {
        return access(req);
    } else {
        req.flags = downgradeZeroAllocFlag(req.flags);
        return cc->passZeroAllocReqToParent(req);
    }
}

uint64_t Cache::access(MemReq& req) {
    assert(!((req.flags & MemReq::ZERO_ALLOC_2) || (req.flags & MemReq::ZERO_ALLOC_3)));
#ifdef MAXSIM_ENABLED
#ifdef RANGE_ACCESSES_PROFILING_ENABLED
    auto addressRange = MaxSimRuntimeInfo::getInst().getRegisteredAddressRange(
            req.lineAddr << ilog2(zinfo->lineSize), MaxSimRuntimeInfo::MaxineAddressSpace_t::Global).type;
    //info("Addr %lx", req.lineAddr << ilog2(zinfo->lineSize));
    switch(req.type) {
        case GETS:
            profGETSByRanges.inc(addressRange); break;
        case GETX:
            profGETXByRanges.inc(addressRange); break;
        case PUTS:
            profPUTSByRanges.inc(addressRange); break;
        case PUTX:
            profPUTXByRanges.inc(addressRange); break;
    }
#endif
#endif // MAXSIM_ENABLED
    uint64_t respCycle = req.cycle;
    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t lineId = array->lookup(req.lineAddr, &req, updateReplacement, false);
        if (req.flags & MemReq::CACHE_SCRUB_1){
            assert(lineId != -1);
            assert(req.type == PUTS);
        }

        if (lineId == -1) { // a miss
            respCycle += tagLat;
        } else { // a hit
            respCycle += accLat;
        }

        if (lineId == -1 && cc->shouldAllocate(req)) {
            if (req.flags & MemReq::CACHE_SCRUB_1) {
                assert(false); //given this is inclusive cache, we should not hit here during WB
            }
            //Make space for new line
            Address wbLineAddr;
            lineId = array->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

            //Evictions are not in the critical path in any sane implementation -- we do not include their delays
            //NOTE: We might be "evicting" an invalid line for all we know. Coherence controllers will know what to do
            cc->processEviction(req, wbLineAddr, lineId, respCycle); //1. if needed, send invalidates/downgrades to lower level

            array->postinsert(req.lineAddr, &req, lineId); //do the actual insertion. NOTE: Now we must split insert into a 2-phase thing because cc unlocks us.
        }

        // Enforce single-record invariant: Writeback access may have a timing
        // record. If so, read it.
        EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
        TimingRecord wbAcc;
        wbAcc.clear();
        if (unlikely(evRec && evRec->hasRecord())) {
            wbAcc = evRec->popRecord();
        }

        respCycle = cc->processAccess(req, lineId, respCycle);

#ifdef CLU_STATS_ENABLED
        array->processAccessCLUStats(req, lineId);
#endif

        // Access may have generated another timing record. If *both* access
        // and wb have records, stitch them together
        if (unlikely(wbAcc.isValid())) {
            if (!evRec->hasRecord()) {
                // Downstream should not care about endEvent for PUTs
                wbAcc.endEvent = nullptr;
                evRec->pushRecord(wbAcc);
            } else {
                // Connect both events
                TimingRecord acc = evRec->popRecord();
                assert(wbAcc.reqCycle >= req.cycle);
                assert(acc.reqCycle >= req.cycle);
                DelayEvent* startEv = new (evRec) DelayEvent(0);
                DelayEvent* dWbEv = new (evRec) DelayEvent(wbAcc.reqCycle - req.cycle);
                DelayEvent* dAccEv = new (evRec) DelayEvent(acc.reqCycle - req.cycle);
                startEv->setMinStartCycle(req.cycle);
                dWbEv->setMinStartCycle(req.cycle);
                dAccEv->setMinStartCycle(req.cycle);
                startEv->addChild(dWbEv, evRec)->addChild(wbAcc.startEvent, evRec);
                startEv->addChild(dAccEv, evRec)->addChild(acc.startEvent, evRec);

                acc.reqCycle = req.cycle;
                acc.startEvent = startEv;
                // endEvent / endCycle stay the same; wbAcc's endEvent not connected
                evRec->pushRecord(acc);
            }
        }
    }
    cc->endAccess(req);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void Cache::startInvalidate() {
    cc->startInv(); //note we don't grab tcc; tcc serializes multiple up accesses, down accesses don't see it
}

uint64_t Cache::finishInvalidate(const InvReq& req) {
    int32_t lineId = array->lookup(req.lineAddr, nullptr, false, req.type == INV || IsCacheScrubbingInv(req.type));
    if (req.type == CLINV){
        uint64_t respCycle;
        if (lineId == -1) {
            respCycle = req.cycle;
        } else {
            respCycle = req.cycle + invLat;
        }
        respCycle = cc->processInv(req, lineId, respCycle); //send invalidates or downgrades to children, and adjust our own state
        return respCycle;
    } else {
        // original, unmodified version
        assert_msg(lineId != -1, "[%s] Invalidate on non-existing address 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
        uint64_t respCycle = req.cycle + invLat;
        trace(Cache, "[%s] Invalidate start 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
        respCycle = cc->processInv(req, lineId, respCycle); //send invalidates or downgrades to children, and adjust our own state
        trace(Cache, "[%s] Invalidate end 0x%lx type %s lineId %d, reqWriteback %d, latency %ld", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback, respCycle - req.cycle);
        return respCycle;
    }
}

uint64_t Cache::scrubInvalidate(MemReq& req) {
    if (!(req.flags & MemReq::CACHE_SCRUB_1)) {
        req.flags = downgradeScrubInvFlag(req.flags);
        return cc->passScrubInvReqToParent(req);
    }

    uint64_t respCycle = req.cycle;

    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    assert(!skipAccess);
    if (likely(!skipAccess)) {
        if (req.flags & MemReq::CACHE_SCRUB_1) {
            int32_t lineId = array->lookup(req.lineAddr, nullptr, false, true);
            if (lineId == -1) { // a miss
                //respCycle += tagLat;
            } else { // a hit
                respCycle += invLat;

                profClInv.inc();
                //Make space for new line
                Address wbLineAddr = req.lineAddr;
                //assert(req.flags && MemReq::CACHE_SCRUB_1);
                req.flags = req.flags |  MemReq::CACHE_SCRUB_1;

                //Evictions are not in the critical path in any sane implementation -- we do not include their delays
                //NOTE: We might be "evicting" an invalid line for all we know. Coherence controllers will know what to do
                cc->processEviction(req, wbLineAddr, lineId, respCycle); //1. if needed, send invalidates/downgrades to lower level
                array->postinsert(-1, &req, lineId); // hack to invalidate the record in the array. I think it is going to mess up the LRU...
                assert(respCycle == req.cycle + invLat);
                assert(!(cc->isValid(lineId)));
                assert(array->lookup(req.lineAddr, nullptr, false, false) == -1);
            }
        } else { assert(false); }
    }

    cc->endAccess(req);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}
