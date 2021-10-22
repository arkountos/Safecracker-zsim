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

#include "simple_core.h"
#include "filter_cache.h"
#include "zsim.h"
#include "pointer_tagging.h"

#ifdef MAXSIM_ENABLED
#include "maxsim/maxsim_profiling.h"
#include "maxsim/maxsim_address_space_morphing.h"
#include "maxsim/maxsim_runtime_info.h"
#endif // MAXSIM_ENABLED

SimpleCore::SimpleCore(FilterCache* _l1i, FilterCache* _l1d, g_string& _name) : Core(_name), l1i(_l1i), l1d(_l1d), instrs(0), curCycle(0), haltedCycles(0), isCondBrunch(false), doSimulateBbl(true), maNum(0) {
#ifdef MA_PROF_ENABLED
    curBblAddr = UNDEF_VIRTUAL_ADDRESS;
#endif
}

void SimpleCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");
    auto x = [this]() -> uint64_t { assert(curCycle >= haltedCycles); return curCycle - haltedCycles; };
    auto cyclesStat = makeLambdaStat(x);
    cyclesStat->init("cycles", "Simulated cycles");
    ProxyStat* instrsStat = new ProxyStat();
    instrsStat->init("instrs", "Simulated instructions", &instrs);
    ProxyStat* zeroAllocInstrsStat = new ProxyStat();
    zeroAllocInstrsStat->init("zeroAllocInstrs", "Simulated instructions during Zero Alloc", &zeroAllocInstrs);
    ProxyStat* zeroAllocCyclesStat = new ProxyStat();
    zeroAllocCyclesStat->init("zeroAllocCycles", "Simulated cycles during Zero Alloc", &zeroAllocCycles);
    coreStat->append(cyclesStat);
    coreStat->append(instrsStat);
    coreStat->append(zeroAllocInstrsStat);
    coreStat->append(zeroAllocCyclesStat);
    parentStat->append(coreStat);
}

uint64_t SimpleCore::getPhaseCycles() const {
    return curCycle - zinfo->globPhaseCycles;
}

void SimpleCore::load(Address addr, MASize_t size, Address base, uint32_t baseReg) {
    assert(maNum < MA_NUM_MAX);
    if (maNum == MA_NUM_MAX) panic("Memory access storage is out of bounds!");
    maAddr[maNum] = addr;
    maSize[maNum] = size;
    maBase[maNum] = base;
    maBaseReg[maNum] = baseReg;
    maIsLoad[maNum++] = true;
    maIsRegWrite[regMaNum++] = false;
    assert(regMaNum <= MA_NUM_MAX * 5);
    curBblMemOp++;
}

void SimpleCore::loadSim(Address addr, MASize_t size, Address base, uint32_t baseReg) {
#ifdef MA_PROF_ENABLED
#   ifdef POINTER_TAGGING_ENABLED
    PointerTag_t tag = getPointerTag(base);
    if (isTagNative(tag)) {
        tag = UNDEF_TAG;
    }
#   else
    PointerTag_t tag = UNDEF_TAG;
#   endif
    MAOffset_t offset = addr - base;

    addr = getUntaggedPointerSE(addr);
    base = getUntaggedPointerSE(base);
#   ifdef MAXSIM_ENABLED
    MaxSimRuntimeInfo::getInst().adjustTagAndOffset(tag, offset, addr);
    addr = MaxSimAddressSpaceMorphing::getInst().processMAAddressAndRemap(addr, base, offset, tag);
    MaxSimProfiling::getInst().addMemoryAccess(tag, offset, curBblAddr, false);
#   else
    UNUSED_VAR(tag); UNUSED_VAR(offset); UNUSED_VAR(curBblAddr);
#   endif
#endif // MA_PROF_ENABLED


    //info("loadSim() addr %lx", addr);

    curCycle = l1d->load(addr, curCycle
#ifdef CLU_STATS_ENABLED
                         , size, LoadData
#endif
#ifdef MA_PROF_ENABLED
                         , tag, offset, curBblAddr
#endif
                         , curTid
                         );
}

void SimpleCore::store(Address addr, MASize_t size, Address base, uint32_t baseReg) {
    assert(maNum < MA_NUM_MAX);
    if (maNum == MA_NUM_MAX) panic("Memory access storage is out of bounds!");
    maAddr[maNum] = addr;
    maSize[maNum] = size;
    maBase[maNum] = base;
    maBaseReg[maNum] = baseReg;
    maIsLoad[maNum++] = false;
    maIsRegWrite[regMaNum++] = false;
    assert(regMaNum <= MA_NUM_MAX * 5);
    curBblMemOp++;
}

void SimpleCore::afterStore() {
    //auto addr = maAddr[maNum];
    //uint64_t value = *((uint64_t *) addr);
    //info("After store: addr %lx value %lx", addr, value);
}

void SimpleCore::storeSim(Address addr, MASize_t size, Address base, uint32_t baseReg) {
#ifdef MA_PROF_ENABLED
#   ifdef POINTER_TAGGING_ENABLED
    PointerTag_t tag = getPointerTag(base);
    if (isTagNative(tag)) {
        tag = UNDEF_TAG;
    }
#   else
    PointerTag_t tag = UNDEF_TAG;
#   endif
    MAOffset_t offset = addr - base;

    addr = getUntaggedPointerSE(addr);
    base = getUntaggedPointerSE(base);
#   ifdef MAXSIM_ENABLED
    MaxSimRuntimeInfo::getInst().adjustTagAndOffset(tag, offset, addr);
    addr = MaxSimAddressSpaceMorphing::getInst().processMAAddressAndRemap(addr, base, offset, tag);
    MaxSimProfiling::getInst().addMemoryAccess(tag, offset, curBblAddr, true);
#   else
    UNUSED_VAR(tag); UNUSED_VAR(offset); UNUSED_VAR(curBblAddr);
#   endif
#endif // MA_PROF_ENABLED
    //uint64_t value = *((uint64_t *) addr);

    curCycle = l1d->store(addr, curCycle
#ifdef CLU_STATS_ENABLED
                          , size
#endif
#ifdef MA_PROF_ENABLED
                          , tag, offset, curBblAddr
#endif
                          , curTid, 0
                          );
}

void SimpleCore::regWrite(uint32_t destReg, Address valueInBaseReg, int64_t offset, uint32_t sourceReg, bool isClear) {
    //info("[Simple Core] reg %i value %lx", destReg, value);
    maWrittenReg[regMaNum] = destReg;
    maWrittenRegBaseValue[regMaNum] = valueInBaseReg;
    maWrittenRegOffset[regMaNum] = offset;
    maWrittenRegSourceReg[regMaNum] = sourceReg;
    maWrittenRegIsClear[regMaNum] = isClear;
    maIsRegWrite[regMaNum++] = true;
    // TODO: delete this if performance hit
    assert(regMaNum < MA_NUM_MAX * 5);
}

//value here is the best guess at what the original calling object pointer offset's address was
void SimpleCore::regWriteSim(uint32_t destReg, Address valueInBaseReg, int64_t offset, uint32_t sourceReg, bool isClear) {
    return;
}

void SimpleCore::bbl(THREADID tid, Address bblAddr, BblInfo* bblInfo, bool inZeroAlloc) {
#ifdef MAXSIM_ENABLED
    doSimulateBbl = MaxSimAddressSpaceMorphing::getInst().processBBlAndDoSimulate(tid, bblAddr, isCondBrunch);
    if (!doSimulateBbl || zinfo->fastForwardThreadTids->at(tid)) {
        //if (zinfo->fastForwardThreadTids->at(tid)) info("Skip bbl since tid %d still in safepointFF",tid);
        maNum = 0;
        regMaNum = 0;
        isCondBrunch = false;
        return;
    }
#endif // MAXSIM_ENABLED

    //for (int i = 0; i < maNum; i++) {
    int i = 0;
    for (int opInd = 0; opInd < regMaNum; opInd++) {
        if (maIsRegWrite[opInd]) {
            regWriteSim(maWrittenReg[opInd], maWrittenRegBaseValue[opInd], maWrittenRegOffset[opInd],
                        maWrittenRegSourceReg[opInd], maWrittenRegIsClear[opInd]);
        } else {
            if (maIsLoad[i]) {
                loadSim(maAddr[i], maSize[i], maBase[i], maBaseReg[i]);
                i++;
            } else {
                storeSim(maAddr[i], maSize[i], maBase[i], maBaseReg[i]);
                i++;
            }
        }
    }
    //info("BBL %s %p", name.c_str(), bblInfo);
    //info("%d %d", bblInfo->instrs, bblInfo->bytes);
    instrs += bblInfo->instrs;
    curCycle += bblInfo->instrs;


#ifdef MA_PROF_ENABLED
    curBblAddr = bblAddr;
#endif
    maNum = 0;
    regMaNum = 0;
    isCondBrunch = false;

    curBblAddr = bblAddr;
    curBblMemOp = 0;

    Address endBblAddr = bblAddr + bblInfo->bytes;
    for (Address fetchAddr = bblAddr; fetchAddr < endBblAddr; fetchAddr+=(1 << lineBits)) {
#ifdef MAXSIM_ENABLED
        MaxSimProfiling::getInst().addMemoryAccess(FETCH_TAG, UNDEF_OFFSET, bblAddr, false);
#endif
        curCycle = l1i->load(fetchAddr, curCycle
#ifdef CLU_STATS_ENABLED
                             , (1 << lineBits), FetchRightPath
#endif
#ifdef MA_PROF_ENABLED
                             , FETCH_TAG, UNDEF_OFFSET, curBblAddr
#endif
                             , curTid
                             );
        curBblMemOp++;
    }
}

void SimpleCore::contextSwitch(int32_t gid) {
    if (gid == -1) {
#ifdef MA_PROF_ENABLED
        curBblAddr = UNDEF_VIRTUAL_ADDRESS;
#endif
        l1i->contextSwitch();
        l1d->contextSwitch();
    }
}

void SimpleCore::join() {
    //info("[%s] Joining, curCycle %ld phaseEnd %ld haltedCycles %ld", name.c_str(), curCycle, phaseEndCycle, haltedCycles);
    if (curCycle < zinfo->globPhaseCycles) { //carry up to the beginning of the phase
        haltedCycles += (zinfo->globPhaseCycles - curCycle);
        curCycle = zinfo->globPhaseCycles;
    }
    phaseEndCycle = zinfo->globPhaseCycles + zinfo->phaseLength;
    //note that with long events, curCycle can be arbitrarily larger than phaseEndCycle; however, it must be aligned in current phase
    //info("[%s] Joined, curCycle %ld phaseEnd %ld haltedCycles %ld", name.c_str(), curCycle, phaseEndCycle, haltedCycles);
}


//Static class functions: Function pointers and trampolines

InstrFuncPtrs SimpleCore::GetFuncPtrs() {
    return {LoadFunc, StoreFunc, BblFunc, BranchFunc, PredLoadFunc, PredStoreFunc, AfterStoreFunc, FPTR_ANALYSIS, RegWriteFunc};
}

void SimpleCore::LoadFunc(THREADID tid, ADDRINT addr, UINT32 size, ADDRINT base, REG baseReg) {
    assert(size < 256);
    static_cast<SimpleCore*>(cores[tid])->load(addr, (MASize_t)size, base, baseReg);
}

void SimpleCore::StoreFunc(THREADID tid, ADDRINT addr, UINT32 size, ADDRINT base, REG baseReg) {
    assert(size < 256);
    static_cast<SimpleCore*>(cores[tid])->store(addr, (MASize_t)size, base, baseReg);
}

void SimpleCore::AfterStoreFunc(THREADID tid) {
    static_cast<SimpleCore*>(cores[tid])->afterStore();
}

void SimpleCore::PredLoadFunc(THREADID tid, ADDRINT addr, UINT32 size, ADDRINT base, BOOL pred, REG baseReg) {
    assert(size < 256);
    if (pred) static_cast<SimpleCore*>(cores[tid])->load(addr, (MASize_t)size, base, baseReg);
}

void SimpleCore::PredStoreFunc(THREADID tid, ADDRINT addr, UINT32 size, ADDRINT base, BOOL pred, REG baseReg) {
    assert(size < 256);
    if (pred) static_cast<SimpleCore*>(cores[tid])->store(addr, (MASize_t)size, base, baseReg);
}

void SimpleCore::BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    SimpleCore* core = static_cast<SimpleCore*>(cores[tid]);
    core->setCurTid(tid);
    core->bbl(tid, bblAddr, bblInfo, false);

    while (core->curCycle > core->phaseEndCycle) {
        assert(core->phaseEndCycle == zinfo->globPhaseCycles + zinfo->phaseLength);
        core->phaseEndCycle += zinfo->phaseLength;

        uint32_t cid = getCid(tid);
        //NOTE: TakeBarrier may take ownership of the core, and so it will be used by some other thread. If TakeBarrier context-switches us,
        //the *only* safe option is to return inmmediately after we detect this, or we can race and corrupt core state. If newCid == cid,
        //we're not at risk of racing, even if we were switched out and then switched in.
        uint32_t newCid = TakeBarrier(tid, cid);
        if (newCid != cid) break; /*context-switch*/
    }
}

void SimpleCore::RegWriteFunc(THREADID tid, REG destReg, ADDRINT valueInBaseReg, INT64 offset, REG sourceReg, BOOL isClear) {
}

void SimpleCore::zeroAllocRegion(Address base, uint32_t numWords, uint32_t zeroAllocLevel, MASize_t size) {
}

void SimpleCore::cacheScrubInvRegion(Address begin, Address end, uint32_t invalidateLevel, THREADID tid) {
}
