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

#include "ooo_core.h"
#include <algorithm>
#include <queue>
#include <string>
#include "bithacks.h"
#include "decoder.h"
#include "filter_cache.h"
#include "zsim.h"
#include "stats.h"
#include "pointer_tagging.h"
#include "constants.h"

#ifdef MAXSIM_ENABLED
#include "maxsim/maxsim_profiling.h"
#include "maxsim/maxsim_address_space_morphing.h"
#include "maxsim/maxsim_runtime_info.h"
#endif // MAXSIM_ENABLED

/* Uncomment to induce backpressure to the IW when the load/store buffers fill up. In theory, more detailed,
 * but sometimes much slower (as it relies on range poisoning in the IW, potentially O(n^2)), and in practice
 * makes a negligible difference (ROB backpressures).
 */
//#define LSU_IW_BACKPRESSURE

#define DEBUG_MSG(args...)
//#define DEBUG_MSG(args...) info(args)

// Core parameters
// TODO(dsm): Make OOOCore templated, subsuming these
#if defined(SILVERMONT)
// NOTE: Changed for KNL (shorter 2-way pipe)
#define FETCH_STAGE 1
#define DECODE_STAGE 3  // NOTE: Decoder adds predecode delays to decode
#define ISSUE_STAGE 5
#define DISPATCH_STAGE 8  // RAT + ROB + RS, each is easily 1-2 cycles
#define L1D_LAT 1  // fixed, and FilterCache does not include L1 dela
//#define L1D_LAT 2
#define FETCH_BYTES_PER_CYCLE 8
#define ISSUES_PER_CYCLE 2
#define RF_READS_PER_CYCLE 2
// end of silvermont
#elif defined(NEHALEM)
// Stages --- more or less matched to Westmere, but have not seen detailed pipe diagrams anywhare
#define FETCH_STAGE 1
#define DECODE_STAGE 4  // NOTE: Decoder adds predecode delays to decode
#define ISSUE_STAGE 7
#define DISPATCH_STAGE 13  // RAT + ROB + RS, each is easily 1-2 cycles
//#define L1D_LAT 4  // fixed, and FilterCache does not include L1 delay
#define L1D_LAT 2  // fixed, and FilterCache does not include L1 delay
#define FETCH_BYTES_PER_CYCLE 16
#define ISSUES_PER_CYCLE 4
#define RF_READS_PER_CYCLE 3
// end of nehalem
#elif defined(HASWELL)
// copy everything from Nehalem except fetch bandwidth
#define FETCH_STAGE 1
#define DECODE_STAGE 4  // NOTE: Decoder adds predecode delays to decode
#define ISSUE_STAGE 7
#define DISPATCH_STAGE 13  // RAT + ROB + RS, each is easily 1-2 cycles
#define L1D_LAT 2  // fixed, and FilterCache does not include L1 delay
#define FETCH_BYTES_PER_CYCLE 32
#define ISSUES_PER_CYCLE 4
#define RF_READS_PER_CYCLE 3
// end of haswell
#elif defined(SKYLAKE)
// copy everything from Nehalem except fetch bandwidth
#define FETCH_STAGE 1
#define DECODE_STAGE 4  // NOTE: Decoder adds predecode delays to decode
#define ISSUE_STAGE 7
#define DISPATCH_STAGE 13  // RAT + ROB + RS, each is easily 1-2 cycles
#define L1D_LAT 4  // fixed, and FilterCache does not include L1 delay
#define FETCH_BYTES_PER_CYCLE 32
#define ISSUES_PER_CYCLE 4
#define RF_READS_PER_CYCLE 3
// end of skylake

#endif
OOOCore::OOOCore(FilterCache* _l1i, FilterCache* _l1d, g_string& _name) : Core(_name), l1i(_l1i), l1d(_l1d), cRec(0, _name) {
    decodeCycle = DECODE_STAGE;  // allow subtracting from it
    curCycle = 0;
    phaseEndCycle = zinfo->phaseLength;

    for (uint32_t i = 0; i < MAX_REGISTERS; i++) {
        regScoreboard[i] = 0;
    }
    prevBbl = nullptr;
#ifdef MA_PROF_ENABLED
    prevBblAddr = UNDEF_VIRTUAL_ADDRESS;
#endif

    lastStoreCommitCycle = 0;
    maxCommitCycle = 0;
    lastStoreAddrCommitCycle = 0;
    curCycleRFReads = 0;
    curCycleIssuedUops = 0;
    branchPc = 0;

    instrs = uops = branchUops = fpAddSubUops = fpMulDivUops = bbls = approxInstrs = mispredBranches = predBranches = 0;

    for (uint32_t i = 0; i < FWD_ENTRIES; i++) fwdArray[i].set((Address)(UNDEF_VIRTUAL_ADDRESS), 0);
}

void OOOCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");

    auto x = [this]() { return cRec.getUnhaltedCycles(curCycle); };
    LambdaStat<decltype(x)>* cyclesStat = new LambdaStat<decltype(x)>(x);
    cyclesStat->init("cycles", "Simulated unhalted cycles");

    auto y = [this]() { return cRec.getContentionCycles(); };
    LambdaStat<decltype(y)>* cCyclesStat = new LambdaStat<decltype(y)>(y);
    cCyclesStat->init("cCycles", "Cycles due to contention stalls");

    ProxyStat* instrsStat = new ProxyStat();
    instrsStat->init("instrs", "Simulated instructions", &instrs);
    ProxyStat* uopsStat = new ProxyStat();
    uopsStat->init("uops", "Retired micro-ops", &uops);
    ProxyStat* branchUopsStat = new ProxyStat();
    branchUopsStat->init("branchUops", "Retired branch micro-ops", &branchUops);
    ProxyStat* fpAddSubUopsStat = new ProxyStat();
    fpAddSubUopsStat->init("fpAddSubUops", "Retired floating point add and sub micro-ops", &fpAddSubUops);
    ProxyStat* fpMulDivUopsStat = new ProxyStat();
    fpMulDivUopsStat->init("fpMulDivUops", "Retired floating point mul and div micro-ops", &fpMulDivUops);
    ProxyStat* bblsStat = new ProxyStat();
    bblsStat->init("bbls", "Basic blocks", &bbls);
    ProxyStat* approxInstrsStat = new ProxyStat();
    approxInstrsStat->init("approxInstrs", "Instrs with approx uop decoding", &approxInstrs);
    ProxyStat* mispredBranchesStat = new ProxyStat();
    mispredBranchesStat->init("mispredBranches", "Mispredicted branches", &mispredBranches);
    ProxyStat* predBranchesStat = new ProxyStat();
    predBranchesStat->init("predBranches", "Predicted branches", &predBranches);

    ProxyStat* zeroAllocInstrsStat = new ProxyStat();
    zeroAllocInstrsStat->init("zeroAllocInstrs", "Simulated instructions during Zero Alloc", &zeroAllocInstrs);
    ProxyStat* zeroAllocUopsStat = new ProxyStat();
    zeroAllocUopsStat->init("zeroAllocUops", "Retired micro-ops during Zero Alloc", &zeroAllocUops);
    ProxyStat* zeroAllocBranchUopsStat = new ProxyStat();
    zeroAllocBranchUopsStat->init("zeroAllocBranchUops", "Retired branch micro-ops during Zero Alloc",
                              &zeroAllocBranchUops);
    ProxyStat* zeroAllocFpAddSubUopsStat = new ProxyStat();
    zeroAllocFpAddSubUopsStat->init("zeroAllocFpAddSubUops",
                                    "Retired floating point add and sub micro-ops during Zero Alloc",
                                    &zeroAllocFpAddSubUops);
    ProxyStat* zeroAllocFpMulDivUopsStat = new ProxyStat();
    zeroAllocFpMulDivUopsStat->init("zeroAllocFpMulDivUops",
                                    "Retired floating point mul and div micro-ops during Zero Alloc",
                                    &zeroAllocFpMulDivUops);
    ProxyStat* zeroAllocBblsStat = new ProxyStat();
    zeroAllocBblsStat->init("zeroAllocBbls", "Basic blocks during Zero Alloc", &zeroAllocBbls);
    ProxyStat* zeroAllocApproxInstrsStat = new ProxyStat();
    zeroAllocApproxInstrsStat->init("zeroAllocApproxInstrs", "Instrs with approx uop decoding during Zero Alloc",
                                    &zeroAllocApproxInstrs);


    profErrOOOLoads.init("errOOOLoads", "# of times that loadIdx != loads");
    coreStat->append(&profErrOOOLoads);
    profErrOOOStores.init("errOOOStores",  "# of times that storeIdx != stores");
    coreStat->append(&profErrOOOStores);

    coreStat->append(cyclesStat);
    coreStat->append(cCyclesStat);
    coreStat->append(instrsStat);
    coreStat->append(uopsStat);
    coreStat->append(branchUopsStat);
    coreStat->append(fpAddSubUopsStat);
    coreStat->append(fpMulDivUopsStat);
    coreStat->append(bblsStat);
    coreStat->append(approxInstrsStat);
    coreStat->append(mispredBranchesStat);
    coreStat->append(predBranchesStat);
    coreStat->append(zeroAllocInstrsStat);
    coreStat->append(zeroAllocUopsStat);
    coreStat->append(zeroAllocBranchUopsStat);
    coreStat->append(zeroAllocFpAddSubUopsStat);
    coreStat->append(zeroAllocFpMulDivUopsStat);
    coreStat->append(zeroAllocBblsStat);
    coreStat->append(zeroAllocApproxInstrsStat);

#ifdef OOO_STALL_STATS
    profFetchStalls.init("fetchStalls",  "Fetch stalls");  coreStat->append(&profFetchStalls);
    profDecodeStalls.init("decodeStalls", "Decode stalls"); coreStat->append(&profDecodeStalls);
    profIssueStalls.init("issueStalls",  "Issue stalls");  coreStat->append(&profIssueStalls);
#endif

    parentStat->append(coreStat);
}

uint64_t OOOCore::getInstrs() const {return instrs;}
//uint64_t OOOCore::getPhaseCycles() const {return maxCommitCycle % zinfo->phaseLength;}
uint64_t OOOCore::getPhaseCycles() const {return maxCommitCycle - zinfo->globPhaseCycles;}

void OOOCore::contextSwitch(int32_t gid) {
    if (gid == -1) {
        // Do not execute previous BBL, as we were context-switched
        prevBbl = nullptr;
#ifdef MA_PROF_ENABLED
        prevBblAddr = UNDEF_VIRTUAL_ADDRESS;
#endif

        // Invalidate virtually-addressed filter caches
        l1i->contextSwitch();
        l1d->contextSwitch();
    }
}


//InstrFuncPtrs OOOCore::GetFuncPtrs() {return {LoadFunc, StoreFunc, BblFunc, BranchFunc, PredLoadFunc, PredStoreFunc, FPTR_ANALYSIS, {0}};}
InstrFuncPtrs OOOCore::GetFuncPtrs() {return {LoadFunc, StoreFunc, BblFunc, BranchFunc, PredLoadFunc, PredStoreFunc, AfterStoreFunc, FPTR_ANALYSIS, RegWriteFunc};}

inline void OOOCore::load(Address addr, uint32_t size, Address base, bool inZeroAlloc, uint32_t baseReg) {
    assert(size < LOAD_STORE_RECORD_SIZE);
    if (loads == LOAD_STORE_RECORD_SIZE) panic("Access to loadAddrs is out of bounds!");
#ifdef POINTER_TAGGING_ENABLED
    PointerTag_t tag = getPointerTag(base);
#endif
#ifdef MA_PROF_ENABLED
    MAOffset_t offset = addr - base;

    addr = getUntaggedPointerSE(addr);
    base = getUntaggedPointerSE(base);
    if (isTagNative(tag)) {
        tag = UNDEF_TAG;
    }
#   ifdef MAXSIM_ENABLED
    MaxSimRuntimeInfo::getInst().adjustTagAndOffset(tag, offset, addr);
#   endif
    loadOffset[loads] = offset;
#endif // MA_PROF_ENABLED
#ifdef POINTER_TAGGING_ENABLED
    loadTag[loads] = tag;
#endif
#ifdef CLU_STATS_ENABLED
    loadSizes[loads] = (MASize_t) size;
#endif
    loadInZeroAlloc[loads] = inZeroAlloc;
    loadBase[loads] = base;
    loadBaseReg[loads] = baseReg;
    loadAddrs[loads++] =
#ifdef MAXSIM_ENABLED
        MaxSimAddressSpaceMorphing::getInst().processMAAddressAndRemap(addr, base, offset, tag);
#else
        addr;
#endif
    maIsRegWrite[regMaNum++] = false;
}

void OOOCore::store(Address addr, uint32_t size, Address base, bool inZeroAlloc, uint32_t baseReg) {
    assert(size < LOAD_STORE_RECORD_SIZE);
    if (stores == LOAD_STORE_RECORD_SIZE) panic("Access to storeAddrs is out of bounds!");
#ifdef POINTER_TAGGING_ENABLED
    PointerTag_t tag = getPointerTag(base);
#endif
#ifdef MA_PROF_ENABLED
    MAOffset_t offset = addr - base;

    addr = getUntaggedPointerSE(addr);
    base = getUntaggedPointerSE(base);
    if (isTagNative(tag)) {
        tag = UNDEF_TAG;
    }
#   ifdef MAXSIM_ENABLED
    MaxSimRuntimeInfo::getInst().adjustTagAndOffset(tag, offset, addr);
#   endif
    storeOffset[stores] = offset;
#endif // MA_PROF_ENABLED
#ifdef POINTER_TAGGING_ENABLED
    storeTag[stores] = tag;
#endif
#ifdef CLU_STATS_ENABLED
    storeSizes[stores] = (MASize_t) size;
#endif
    storeInZeroAlloc[stores] = inZeroAlloc;
    storeBase[stores] = base;
    storeBaseReg[stores] = baseReg;
    storeAddrs[stores++] =
#ifdef MAXSIM_ENABLED
        MaxSimAddressSpaceMorphing::getInst().processMAAddressAndRemap(addr, base, offset, tag);
#else
        addr;
#endif
    maIsRegWrite[regMaNum++] = false;
}

// Predicated loads and stores call this function, gets recorded as a 0-cycle op.
// Predication is rare enough that we don't need to model it perfectly to be accurate (i.e. the uops still execute, retire, etc), but this is needed for correctness.
void OOOCore::predFalseMemOp(bool inZeroAlloc) {
    // I'm going to go out on a limb and assume just loads are predicated (this will not fail silently if it's a store)
    loadInZeroAlloc[loads] = inZeroAlloc;
    loadAddrs[loads++] = UNDEF_VIRTUAL_ADDRESS;
}

void OOOCore::branch(Address pc, bool taken, Address takenNpc, Address notTakenNpc) {
    branchPc = pc;
    branchTaken = taken;
    branchTakenNpc = takenNpc;
    branchNotTakenNpc = notTakenNpc;
}

inline void OOOCore::bbl(THREADID tid, Address bblAddr, BblInfo* bblInfo, bool inZeroAlloc) {
    if (!prevBbl) {
        // This is the 1st BBL since scheduled, nothing to simulate
        prevBbl = bblInfo;
#ifdef MA_PROF_ENABLED
        prevBblAddr = bblAddr;
#endif
        // Kill lingering ops from previous BBL
        loads = stores = regMaNum = 0;
        return;
    }

    /* Simulate execution of previous BBL */

    uint32_t bblInstrs = prevBbl->instrs;
    DynBbl* bbl = &(prevBbl->oooBbl[0]);
#ifdef MA_PROF_ENABLED
    Address bblIP = prevBblAddr;
#endif
    prevBbl = bblInfo;
#ifdef MA_PROF_ENABLED
    prevBblAddr = bblAddr;
#endif

    uint32_t loadIdx = 0;
    uint32_t storeIdx = 0;
    uint32_t regMaIdx = 0;

    uint32_t prevDecCycle = 0;
    uint64_t lastCommitCycle = 0;  // used to find misprediction penalty

    // Run dispatch/IW
    for (uint32_t i = 0; i < bbl->uops; i++) {
        DynUop* uop = &(bbl->uop[i]);

        // Decode stalls
        uint32_t decDiff = uop->decCycle - prevDecCycle;
        decodeCycle = MAX(decodeCycle + decDiff, uopQueue.minAllocCycle());
        if (decodeCycle > curCycle) {
            //info("Decode stall %ld %ld | %d %d", decodeCycle, curCycle, uop->decCycle, prevDecCycle);
            uint32_t cdDiff = decodeCycle - curCycle;
#ifdef OOO_STALL_STATS
            profDecodeStalls.inc(cdDiff);
#endif
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
            for (uint32_t i = 0; i < cdDiff; i++) insWindow.advancePos(curCycle);
        }
        prevDecCycle = uop->decCycle;
        uopQueue.markLeave(curCycle);

        // Implement issue width limit --- we can only issue 4 uops/cycle
        if (curCycleIssuedUops >= ISSUES_PER_CYCLE) {
#ifdef OOO_STALL_STATS
            profIssueStalls.inc();
#endif
            // info("Advancing due to uop issue width");
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
            insWindow.advancePos(curCycle);
        }
        curCycleIssuedUops++;

        // Kill dependences on invalid register
        // Using curCycle saves us two unpredictable branches in the RF read stalls code
        regScoreboard[0] = curCycle;

        uint64_t c0 = regScoreboard[uop->rs[0]];
        uint64_t c1 = regScoreboard[uop->rs[1]];

        // RF read stalls
        // if srcs are not available at issue time, we have to go thru the RF
        curCycleRFReads += ((c0 < curCycle)? 1 : 0) + ((c1 < curCycle)? 1 : 0);
        if (curCycleRFReads > RF_READS_PER_CYCLE) {
            curCycleRFReads -= RF_READS_PER_CYCLE;
            curCycleIssuedUops = 0;  // or 1? that's probably a 2nd-order detail
            insWindow.advancePos(curCycle);
        }

        uint64_t c2 = rob.minAllocCycle();
        uint64_t c3 = curCycle;

        uint64_t cOps = MAX(c0, c1);

        // Model RAT + ROB + RS delay between issue and dispatch
        uint64_t dispatchCycle = MAX(cOps, MAX(c2, c3) + (DISPATCH_STAGE - ISSUE_STAGE));

        // info("IW 0x%lx %d %ld %ld %x", bblAddr, i, c2, dispatchCycle, uop->portMask);
        // NOTE: Schedule can adjust both cur and dispatch cycles
        insWindow.schedule(curCycle, dispatchCycle, uop->portMask, uop->extraSlots);

        // If we have advanced, we need to reset the curCycle counters
        if (curCycle > c3) {
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
        }

        uint64_t commitCycle;

        //check for regWrites
        if (maIsRegWrite[regMaIdx]) {
            regWriteSim(maWrittenReg[regMaIdx], maWrittenRegBaseValue[regMaIdx], maWrittenRegOffset[regMaIdx],
                        maWrittenRegSourceReg[regMaIdx], maWrittenRegIsClear[regMaIdx]);
            regMaIdx++;
        }

        // LSU simulation
        // NOTE: Ever-so-slightly faster than if-else if-else if-else
        switch (uop->type) {
            case UOP_GENERAL:
                commitCycle = dispatchCycle + uop->lat;
                break;

            case UOP_LOAD:
                {
                    // dispatchCycle = MAX(loadQueue.minAllocCycle(), dispatchCycle);
                    uint64_t lqCycle = loadQueue.minAllocCycle();
                    if (lqCycle > dispatchCycle) {
#ifdef LSU_IW_BACKPRESSURE
                        insWindow.poisonRange(curCycle, lqCycle, 0x4 /*PORT_2, loads*/);
#endif
                        dispatchCycle = lqCycle;
                    }

                    // Wait for all previous store addresses to be resolved
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

#ifdef MA_PROF_ENABLED
                    MAOffset_t offset = loadOffset[loadIdx];
#   ifdef POINTER_TAGGING_ENABLED
                    PointerTag_t tag = loadTag[loadIdx];
#   else
                    PointerTag_t tag = UNDEF_TAG;
#   endif
#endif // MA_PROF_ENABLED
#ifdef CLU_STATS_ENABLED
                    int8_t size = loadSizes[loadIdx];
#endif
                    bool skipLoadInZeroAlloc = loadInZeroAlloc[loadIdx];
                    Address addr = loadAddrs[loadIdx++];
                    regMaIdx++;
                    uint64_t reqSatisfiedCycle = dispatchCycle;
                    if ((addr != UNDEF_VIRTUAL_ADDRESS) && (!skipLoadInZeroAlloc)) {
#ifdef MA_PROF_ENABLED
#   ifdef MAXSIM_ENABLED
                        MaxSimProfiling::getInst().addMemoryAccess(tag, offset, bblIP, false);
#   else
                        UNUSED_VAR(tag); UNUSED_VAR(offset); UNUSED_VAR(bblIP);
#   endif
#endif
                        reqSatisfiedCycle = l1d->load(addr, dispatchCycle
#ifdef CLU_STATS_ENABLED
                                                      , size, LoadData
#endif
#ifdef MA_PROF_ENABLED
                                                      , tag, offset, bblIP
#endif
                                                      , curTid//0 /*FIXME*/
                                                      ) + L1D_LAT;
                        cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);

                        // Enforce st-ld forwarding
                        uint32_t fwdIdx = (addr>>2) & (FWD_ENTRIES-1);
                        if (fwdArray[fwdIdx].addr == addr) {
                            // info("0x%lx FWD %ld %ld", addr, reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                            /* Take the MAX (see FilterCache's code) Our fwdArray
                             * imposes more stringent timing constraints than the
                             * l1d, b/c FilterCache does not change the line's
                             * availCycle on a store. This allows FilterCache to
                             * track per-line, not per-word availCycles.
                             */
                            reqSatisfiedCycle = MAX(reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                        }
                    }

                    commitCycle = reqSatisfiedCycle;
                    loadQueue.markRetire(commitCycle);
                }
                break;

            case UOP_STORE:
                {
                    // dispatchCycle = MAX(storeQueue.minAllocCycle(), dispatchCycle);
                    uint64_t sqCycle = storeQueue.minAllocCycle();
                    if (sqCycle > dispatchCycle) {
#ifdef LSU_IW_BACKPRESSURE
                        insWindow.poisonRange(curCycle, sqCycle, 0x10 /*PORT_4, stores*/);
#endif
                        dispatchCycle = sqCycle;
                    }

                    // Wait for all previous store addresses to be resolved (not just ours :))
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

#ifdef MA_PROF_ENABLED
                    MAOffset_t offset = storeOffset[storeIdx];
#   ifdef POINTER_TAGGING_ENABLED
                    PointerTag_t tag = storeTag[storeIdx];
#   else
                    PointerTag_t tag = UNDEF_TAG;
#   endif
#endif // MA_PROF_ENABLED
#ifdef CLU_STATS_ENABLED
                    int8_t size = storeSizes[storeIdx];
#endif
                    bool skipStoreInZeroAlloc = storeInZeroAlloc[storeIdx];
                    Address addr = storeAddrs[storeIdx++];
                    regMaIdx++;

                    uint64_t reqSatisfiedCycle = dispatchCycle;
                    if ((addr != UNDEF_VIRTUAL_ADDRESS) && (!skipStoreInZeroAlloc)) {
#ifdef MA_PROF_ENABLED
#   ifdef MAXSIM_ENABLED
                        MaxSimProfiling::getInst().addMemoryAccess(tag, offset, bblIP, true);
#   else
                        UNUSED_VAR(tag); UNUSED_VAR(offset); UNUSED_VAR(bblIP);
#   endif
#endif
                        reqSatisfiedCycle = l1d->store(addr, dispatchCycle
#ifdef CLU_STATS_ENABLED
                                                       , size
#endif
#ifdef MA_PROF_ENABLED
                                                       , tag, offset, bblIP
#endif
                                                       , 0, curTid//0 /*FIXME*/
                                                       ) + L1D_LAT;
                        cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);

                        // Fill the forwarding table
                        fwdArray[(addr>>2) & (FWD_ENTRIES-1)].set(addr, reqSatisfiedCycle);
                    }
                    commitCycle = reqSatisfiedCycle;
                    lastStoreCommitCycle = MAX(lastStoreCommitCycle, reqSatisfiedCycle);
                    storeQueue.markRetire(commitCycle);
                }
                break;

            case UOP_STORE_ADDR:
                commitCycle = dispatchCycle + uop->lat;
                lastStoreAddrCommitCycle = MAX(lastStoreAddrCommitCycle, commitCycle);
                break;

            //case UOP_FENCE:  //make gcc happy
            default:
                assert((UopType) uop->type == UOP_FENCE || (UopType) uop->type == UOP_RDTSC);
                commitCycle = dispatchCycle + uop->lat;
                // info("%d %ld %ld", uop->lat, lastStoreAddrCommitCycle, lastStoreCommitCycle);
                // force future load serialization
                lastStoreAddrCommitCycle = MAX(commitCycle, MAX(lastStoreAddrCommitCycle, lastStoreCommitCycle + uop->lat));
                // info("%d %ld %ld X", uop->lat, lastStoreAddrCommitCycle, lastStoreCommitCycle);
                if ((UopType) uop->type == UOP_RDTSC) {
                    auto maxC = MAX(maxCommitCycle, commitCycle);
                    //info("RDTSC: curCycle %lu advance to cycle %lu", curCycle, maxC);
                    advance(maxC);
                }
        }

        // Mark retire at ROB
        rob.markRetire(commitCycle);

        // Record dependences
        regScoreboard[uop->rd[0]] = commitCycle;
        regScoreboard[uop->rd[1]] = commitCycle;

        lastCommitCycle = commitCycle;
        maxCommitCycle = MAX(maxCommitCycle, commitCycle);
        //info("0x%lx %3d [%3d %3d] -> [%3d %3d]  %8ld %8ld %8ld %8ld", bbl->addr, i, uop->rs[0], uop->rs[1], uop->rd[0], uop->rd[1], decCycle, c3, dispatchCycle, commitCycle);
    }

    instrs += bblInstrs;
    uops += bbl->uops;
    branchUops += bbl->branchUops;
    fpAddSubUops += bbl->fpAddSubUops;
    fpMulDivUops += bbl->fpMulDivUops;
    bbls++;
    approxInstrs += bbl->approxInstrs;

#ifdef BBL_PROFILING
    if (approxInstrs) Decoder::profileBbl(bbl->bblIdx);
#endif

    // Check full match between expected and actual mem ops
    // If these assertions fail, most likely, something's off in the decoder
    //assert_msg(loadIdx == loads, "%s: loadIdx(%d) != loads (%d)", name.c_str(), loadIdx, loads);
    if (loadIdx != loads) {
        profErrOOOLoads.inc();
    }
    if (storeIdx != stores) {
        profErrOOOStores.inc();
    }
    //assert_msg(storeIdx == stores, "%s: storeIdx(%d) != stores (%d)", name.c_str(), storeIdx, stores);
    loads = stores = regMaNum = 0;


    /* Simulate frontend for branch pred + fetch of this BBL
     *
     * NOTE: We assume that the instruction length predecoder and the IQ are
     * weak enough that they can't hide any ifetch or bpred stalls. In fact,
     * predecoder stalls are incorporated in the decode stall component (see
     * decoder.cpp). So here, we compute fetchCycle, then use it to adjust
     * decodeCycle.
     */

    // Model fetch-decode delay (fixed, weak predec/IQ assumption)
    uint64_t fetchCycle = decodeCycle - (DECODE_STAGE - FETCH_STAGE);
    uint32_t lineSize = 1 << lineBits;

    // Simulate branch prediction
    if (branchPc && !branchPred.predict(branchPc, branchTaken)) {
        mispredBranches++;

        /* Simulate wrong-path fetches
         *
         * This is not for a latency reason, but sometimes it increases fetched
         * code footprint and L1I MPKI significantly. Also, we assume a perfect
         * BTB here: we always have the right address to missfetch on, and we
         * never need resteering.
         *
         * NOTE: Resteering due to BTB misses is done at the BAC unit, is
         * relatively rare, and carries an 8-cycle penalty, which should be
         * partially hidden if the branch is predicted correctly --- so we
         * don't simulate it.
         *
         * Since we don't have a BTB, we just assume the next branch is not
         * taken. With a typical branch mispred penalty of 17 cycles, we
         * typically fetch 3-4 lines in advance (16B/cycle). This sets a higher
         * limit, which can happen with branches that take a long time to
         * resolve (because e.g., they depend on a load). To set this upper
         * bound, assume a completely backpressured IQ (18 instrs), uop queue
         * (28 uops), IW (36 uops), and 16B instr length predecoder buffer. At
         * ~3.5 bytes/instr, 1.2 uops/instr, this is about 5 64-byte lines.
         */

        // info("Mispredicted branch, %ld %ld %ld | %ld %ld", decodeCycle, curCycle, lastCommitCycle,
        //         lastCommitCycle-decodeCycle, lastCommitCycle-curCycle);
        Address wrongPathAddr = branchTaken? branchNotTakenNpc : branchTakenNpc;
        uint64_t reqCycle = fetchCycle;
        for (uint32_t i = 0; i < 5*64/lineSize; i++) {
#ifdef MA_PROF_ENABLED
#   ifdef MAXSIM_ENABLED
            MaxSimProfiling::getInst().addMemoryAccess(FETCH_TAG, UNDEF_OFFSET, bblIP, false);
#   endif
#endif
            uint64_t fetchLat;
            fetchLat = l1i->load(wrongPathAddr + lineSize*i, curCycle
#ifdef CLU_STATS_ENABLED
                                          , lineSize, FetchWrongPath
#endif
#ifdef MA_PROF_ENABLED
                                          , FETCH_TAG, UNDEF_OFFSET, wrongPathAddr
#endif
                                          , curTid//0 /* FIXME */
                                          ) - curCycle;
            cRec.record(curCycle, curCycle, curCycle + fetchLat);
            uint64_t respCycle = reqCycle + fetchLat;
            if (respCycle > lastCommitCycle) {
                break;
            }
            // Model fetch throughput limit
            reqCycle = respCycle + lineSize/FETCH_BYTES_PER_CYCLE;
        }

        fetchCycle = lastCommitCycle;
    } else if (branchPc) {
        predBranches++;
    }
    branchPc = 0;  // clear for next BBL

    // Simulate current bbl ifetch
    Address endAddr = bblAddr + bblInfo->bytes;
    for (Address fetchAddr = bblAddr; fetchAddr < endAddr; fetchAddr += lineSize) {
        // The Nehalem frontend fetches instructions in 16-byte-wide accesses.
        // Do not model fetch throughput limit here, decoder-generated stalls already include it
        // We always call fetches with curCycle to avoid upsetting the weave
        // models (but we could move to a fetch-centric recorder to avoid this)
#ifdef MA_PROF_ENABLED
#   ifdef MAXSIM_ENABLED
        MaxSimProfiling::getInst().addMemoryAccess(FETCH_TAG, UNDEF_OFFSET, bblIP, false);
#   endif
#endif
        uint64_t fetchLat;
        fetchLat = l1i->load(fetchAddr, curCycle
#ifdef CLU_STATS_ENABLED
                                      , lineSize, FetchRightPath
#endif
#ifdef MA_PROF_ENABLED
                                      , FETCH_TAG, UNDEF_OFFSET, bblAddr
#endif
                                      , curTid // 0 /* FIXME */
                                      ) - curCycle;
        cRec.record(curCycle, curCycle, curCycle + fetchLat);
        fetchCycle += fetchLat;
    }

    // If fetch rules, take into account delay between fetch and decode;
    // If decode rules, different BBLs make the decoders skip a cycle
    decodeCycle++;
    uint64_t minFetchDecCycle = fetchCycle + (DECODE_STAGE - FETCH_STAGE);
    if (minFetchDecCycle > decodeCycle) {
#ifdef OOO_STALL_STATS
        profFetchStalls.inc(minFetchDecCycle - decodeCycle);
#endif
        decodeCycle = minFetchDecCycle;
    }
}

// Timing simulation code
void OOOCore::join() {
    DEBUG_MSG("[%s] Joining, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    uint64_t targetCycle = cRec.notifyJoin(curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
    phaseEndCycle = zinfo->globPhaseCycles + zinfo->phaseLength;
    // assert(targetCycle <= phaseEndCycle);
    DEBUG_MSG("[%s] Joined, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
}

void OOOCore::leave() {
    DEBUG_MSG("[%s] Leaving, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    cRec.notifyLeave(curCycle);
}

void OOOCore::cSimStart() {
    uint64_t targetCycle = cRec.cSimStart(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

void OOOCore::cSimEnd() {
    uint64_t targetCycle = cRec.cSimEnd(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

void OOOCore::advance(uint64_t targetCycle) {
    assert(targetCycle > curCycle);
    decodeCycle += targetCycle - curCycle;
    insWindow.longAdvance(curCycle, targetCycle);
    curCycleRFReads = 0;
    curCycleIssuedUops = 0;
    assert(targetCycle == curCycle);
    /* NOTE: Validation with weave mems shows that not advancing internal cycle
     * counters in e.g., the ROB does not change much; consider full-blown
     * rebases though if weave models fail to validate for some app.
     */
}

// Pin interface code

void OOOCore::LoadFunc(THREADID tid, ADDRINT addr, UINT32 size, ADDRINT base, REG baseReg) {
    static_cast<OOOCore*>(cores[tid])->load(addr, size, base, false, baseReg);
}

void OOOCore::StoreFunc(THREADID tid, ADDRINT addr, UINT32 size, ADDRINT base, REG baseReg) {
    static_cast<OOOCore*>(cores[tid])->store(addr, size, base, false, baseReg);
}

void OOOCore::PredLoadFunc(THREADID tid, ADDRINT addr, UINT32 size, ADDRINT base, BOOL pred, REG baseReg) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    if (pred) core->load(addr, size, base, false, baseReg);
    else core->predFalseMemOp(false);
}

void OOOCore::PredStoreFunc(THREADID tid, ADDRINT addr, UINT32 size, ADDRINT base, BOOL pred, REG baseReg) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    if (pred) core->store(addr, size, base, false, baseReg);
    else core->predFalseMemOp(false);
}

void OOOCore::AfterStoreFunc(THREADID tid) {
    return ;
}

void OOOCore::BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    core->setCurTid(tid);
    core->bbl(tid, bblAddr, bblInfo, false);

    while (core->curCycle > core->phaseEndCycle) {
        core->phaseEndCycle += zinfo->phaseLength;

        uint32_t cid = getCid(tid);
        // NOTE: TakeBarrier may take ownership of the core, and so it will be used by some other thread. If TakeBarrier context-switches us,
        // the *only* safe option is to return inmmediately after we detect this, or we can race and corrupt core state. However, the information
        // here is insufficient to do that, so we could wind up double-counting phases.
        uint32_t newCid = TakeBarrier(tid, cid);
        // NOTE: Upon further observation, we cannot race if newCid == cid, so this code should be enough.
        // It may happen that we had an intervening context-switch and we are now back to the same core.
        // This is fine, since the loop looks at core values directly and there are no locals involved,
        // so we should just advance as needed and move on.
        if (newCid != cid) break;  /*context-switch, we do not own this context anymore*/
    }
}

void OOOCore::BranchFunc(THREADID tid, ADDRINT pc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc) {
    static_cast<OOOCore*>(cores[tid])->branch(pc, taken, takenNpc, notTakenNpc);
}

void OOOCore::zeroAllocRegion(Address base, uint32_t numWords, uint32_t zeroAllocLevel, MASize_t size, THREADID tid) {
}

void OOOCore::cacheScrubInvRegion(Address begin, Address end, uint32_t invalidateLevel, THREADID tid) {
}

void OOOCore::RegWriteFunc(THREADID tid, REG destReg, ADDRINT valueInBaseReg, INT64 offset, REG sourceReg, BOOL isClear) {
        static_cast<OOOCore*>(cores[tid])->regWrite(destReg, valueInBaseReg, offset, sourceReg, isClear);
}

void OOOCore::regWrite(uint32_t destReg, Address valueInBaseReg, int64_t offset, uint32_t sourceReg, bool isClear) {
    //info("[OOO Core] reg %i value %lx", destReg, value);
    //assert(false); //untested
    maWrittenReg[regMaNum] = destReg;
    maWrittenRegBaseValue[regMaNum] = valueInBaseReg;
    maWrittenRegOffset[regMaNum] = offset;
    maWrittenRegSourceReg[regMaNum] = sourceReg;
    maWrittenRegIsClear[regMaNum] = isClear;
    maIsRegWrite[regMaNum++] = true;
    // TODO: delete this if performance hit
    assert(regMaNum < REG_ARG_NUM_MAX);
}

void OOOCore::regWriteSim(uint32_t destReg, Address valueInBaseReg, int64_t offset, uint32_t sourceReg, bool isClear) {
}
