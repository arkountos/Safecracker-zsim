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

#include "memory_hierarchy.h"
#include "config.h"
#include "stats.h"
#include "tlb.h"
#include "mutex.h"
#include "g_std/g_unordered_map.h"

typedef g_unordered_map<Address, Address> PageTable;

class VirtualMemory : public GlobAlloc {
    private:
        bool simulateTLB;
        g_vector<TLB*> tlbs;

    protected:
        uint32_t pageSize;
        g_unordered_map<uint64_t, PageTable> pageTables; // per-process page table
        rwmutex pageTableLock;
        Address jvmHeapBase = 0;
        uint64_t jvmHeapUsage = 0;

        ProxyFuncStat profMemoryFootprint;

    public:
        uint64_t getMemoryFootprint();
        VirtualMemory(AggregateStat* parentStat, Config& config);
        virtual uint64_t translate(Address vLineAddr, Address& pLineAddr);
        virtual void setJvmHeapBase(Address addr) {
            jvmHeapBase = addr;
            for (auto& p : pageTables) p.second.clear();
        } 
        virtual void setJvmHeapUsage(uint64_t usage);
        void updateProcMaps(); 
}; // class VirtualMemory

// poan: hackaround to use ProxyFuncStats;
extern uint64_t getMemoryFootprintGlob();

