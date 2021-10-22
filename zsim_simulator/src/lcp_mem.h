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

#include "virtual_mem.h"

/*
 * This class implements the LCP main memory compression technique.
 * See Pekhimenko et. al "Linearly Compressed Pages: A Low-Complexity,
 * Low-Latency Main Memory Compression Framework" for details.
 */

// LCP meta data, fig.7
struct LcpMetaData {
    bool ebit[64];
    uint8_t eindex[64];
    bool vbit[64];
};

// LCP page table entry, fig.5
struct LcpPTE{
    LcpMetaData metaData;
    uint64_t pageSize;
    uint8_t ctype;
    uint8_t csize;
    uint8_t cbase;
    bool cbit;
};

typedef g_unordered_map<Address, LcpPTE> LcpPageTable;

class LcpVirtualMemory : public VirtualMemory {
    private:
        g_unordered_map<uint64_t, LcpPageTable> lcpPageTables;
        uint64_t compressedPageSizes[4] = {512, 1024, 2048, 4096};
        uint64_t compressedLineSizes[9] = {4, 8, 12, 16, 20, 24, 28, 32, 64};
        uint32_t blockSize = 64;
        ProxyFuncStat profCompressedMemoryFootprint;
        Counter profTypeOneOverflow, profTypeTwoOverflow;

    public:
        LcpVirtualMemory(AggregateStat* parentStat, Config& config);
        uint64_t translate(Address vLineAddr, Address& pLineAddr);
        uint64_t getCompressedMemoryFootprint();
        void setJvmHeapBase(Address addr) {
            VirtualMemory::setJvmHeapBase(addr);
            for (auto& p : lcpPageTables) p.second.clear();
        } 
        void setJvmHeapUsage(uint64_t usage);
        void compressPage(Address pageAddr);
        uint64_t dirtyWriteBack(Address lineAddr);
        uint32_t getNumOfFreeLines(Address lineAddr);
}; // class LcpVirtualMemory

// poan: hackaround to use ProxyFuncStats;
extern uint64_t getCompressedMemoryFootprintGlob();

