/** $lic$
 * Copyright (C) 2012-2013 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2012 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * This is an internal version, and is not under GPL. All rights reserved.
 * Only MIT and Stanford students and faculty are allowed to use this version.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2010) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 */

#pragma once

#include "zsim.h"
#include "memory_hierarchy.h"

class TLB : public GlobAlloc {
    private:
        struct TLBEntry {
            bool valid;
            bool sharedPage;
            uint32_t age;
            uint64_t vPageNum;
            uint64_t pPageNum;
            void clear() {valid = false; sharedPage = false; age = 0; vPageNum = 0; pPageNum = 0;}
        };

        uint32_t numEntries, numWays, numSets;
        uint32_t missPenalty;
        uint32_t pageSizeBits;
        //Address pPage, vPage;
        uint64_t tlbHits, tlbMisses;
        TLBEntry* array;
        TLB* parentTLB;

    public:
        TLB(uint32_t _numEntries, uint32_t _numWays, uint32_t _missPenalty, uint32_t _pageSizeBits)
            : numEntries(_numEntries), numWays(_numWays), missPenalty(_missPenalty), pageSizeBits(_pageSizeBits)
        {
            tlbHits = tlbMisses = 0;
            numSets = numEntries / numWays;
            assert(numEntries % numWays == 0);
            array = gm_memalign<TLBEntry>(CACHE_LINE_BYTES, numEntries);
            for (uint32_t i = 0; i < numEntries ; i++) array[i].clear();
            parentTLB = nullptr;
        }

        void initStats(AggregateStat* parentStat, const char* name) {
            AggregateStat* tlbStat = new AggregateStat();
            tlbStat->init(name, name);

            ProxyStat* hitStat = new ProxyStat();
            hitStat->init("hits", "TLB hits", &tlbHits);
            ProxyStat* missStat = new ProxyStat();
            missStat->init("misses", "TLB misses", &tlbMisses);

            tlbStat->append(hitStat);
            tlbStat->append(missStat);
            parentStat->append(tlbStat);
        }

        uint64_t getTLBMissPenalty(){
            return missPenalty;
        }
        // modify pLineAddr and sharedPage as output,
        // return translation cycle
        uint64_t lookup(Address vLineAddr, Address& pLineAddr, bool& sharedPage) { 
            uint64_t vPageNum = vLineAddr >> (pageSizeBits - lineBits);
            uint32_t setIdx = vPageNum % numSets;
            uint64_t pPage = (uint32_t) -1;
            bool found = false;
            for (uint32_t way = 0; way < numWays; way++){
                auto& entry = array[setIdx * numWays + way];
                if (entry.vPageNum == vPageNum && entry.valid) {
                    found = true;
                    pPage = entry.pPageNum;
                    entry.valid = true;
                    entry.age = 0;
                    sharedPage = entry.sharedPage;
                } else {
                    entry.age++;
                }
            }
            uint64_t lookupCycle = 0;
            if (found) {// found the translation
                tlbHits++;
                pLineAddr = (pPage << (pageSizeBits - lineBits)) | // pPage | vAddr lower bits
                    ((vLineAddr << (64 - pageSizeBits + lineBits)) >> (64 - pageSizeBits + lineBits));
            }
            else {
                tlbMisses++;
                lookupCycle += missPenalty;
                if (parentTLB != nullptr) {
                    lookupCycle += parentTLB->lookup(vLineAddr, pLineAddr, sharedPage);
                }
                //else {
                //    pLineAddr = getPhysLineAddr(vLineAddr, &sharedPage);
                //}
                // insert it to this level
                insert(pLineAddr, vLineAddr, sharedPage);
            }
            return lookupCycle;
        }

        void insert(Address pLineAddr, Address vLineAddr, bool sharedPage){ // LRU replacement
            assert(pLineAddr != (uint64_t) -1);
            uint64_t vPageNum = vLineAddr >> (pageSizeBits - lineBits);
            uint32_t setIdx = vPageNum % numSets;
            uint32_t replaceWay = 0;
            uint32_t replaceAge = 0;
            for (uint32_t way = 0; way < numWays; way++){
                auto& entry = array[setIdx * numWays + way];
                if (entry.age >= replaceAge || !entry.valid) {
                    replaceWay = way;
                    replaceAge = entry.age;
                }
            }
            auto& replaceEntry = array[setIdx * numWays + replaceWay];
            //info("TLB: Replace way %u", replaceWay);
            replaceEntry.valid = true;
            replaceEntry.pPageNum = pLineAddr >> (pageSizeBits - lineBits);
            replaceEntry.vPageNum = vLineAddr >> (pageSizeBits - lineBits);
            replaceEntry.age = 0;
            replaceEntry.sharedPage = sharedPage;
        }

        void setParent(TLB* _parentTLB){
            parentTLB = _parentTLB;
        }

        void contextSwitch(){ // TODO:invalid all tlb
            return; 
        }
}; // class TLB
