#include <sstream>

#include "bithacks.h"
#include "virtual_mem.h"
#include "g_std/g_string.h"

// Implement in the .cpp file to avoid multiple definitions during linking
uint64_t getMemoryFootprintGlob() { return zinfo->vm->getMemoryFootprint(); }

uint64_t VirtualMemory::getMemoryFootprint() {
    uint64_t footprint = 0;
    for (auto p : pageTables) {
        footprint += pageSize * p.second.size();
    }
    return footprint;
}

VirtualMemory::VirtualMemory(AggregateStat* parentStat, Config& config) {
    info("Initializing Virtual Memory System");
    string prefix = "sys.vm.";
    simulateTLB = config.get<bool>(prefix + "simulateTLB", false);
    pageSize = config.get<uint32_t>(prefix + "pageSize", 4096);
    if (simulateTLB) {
        auto numEntries = config.get<uint32_t>(prefix + "tlb.entries", 4096);
        auto numWays = config.get<uint32_t>(prefix + "tlb.ways", 8);
        auto missPenalty = config.get<uint32_t>(prefix + "tlb.missPenalty", 100);
        auto pageSizeBits = ilog2(pageSize);
        for (uint32_t c = 0; c < zinfo->numCores; c++) {
            auto tlb = new TLB(numEntries, numWays, missPenalty, pageSizeBits);
            tlbs.push_back(tlb);
        }
    }

    AggregateStat* s = new AggregateStat();
    s->init("vm", "vm stats");
    profMemoryFootprint.init("footprint", "Active memory footprint", &getMemoryFootprintGlob);
    s->append(&profMemoryFootprint);

    if (simulateTLB) {
        for (uint32_t c = 0; c < zinfo->numCores; c++){
            std::stringstream ss;
            ss << "tlb-" << c;
            g_string name(ss.str().c_str());
            tlbs[c]->initStats(s, name.c_str());
        }
    }
    parentStat->append(s);

}

uint64_t VirtualMemory::translate(Address vLineAddr, Address& pLineAddr) {
    pLineAddr = procMask | vLineAddr;
    Address vPageNum = vLineAddr >> ilog2(pageSize / zinfo->lineSize);
    pageTableLock.rdLock();
    if (pageTables.find(procIdx) == pageTables.end()) {
        pageTableLock.upgrade();
        pageTables[procIdx] = PageTable();
        pageTableLock.downgrade();
    }
    auto& pageTable = pageTables[procIdx];
    if (pageTable.find(vPageNum) == pageTable.end()) {
        pageTableLock.upgrade();
        Address pPageNum = pLineAddr >> ilog2(pageSize / zinfo->lineSize); 
        pageTable[vPageNum] = pPageNum;
        pageTableLock.downgrade();
    }
    pageTableLock.rdUnlock();
    return 0;
}

void VirtualMemory::setJvmHeapUsage(uint64_t usage) {
    if (jvmHeapBase == 0) return; // the base is not yet set;

    info("VirtualMemory::setJvmHeapUsage before: memoryfootprint = %lu", getMemoryFootprint());
    pageTableLock.rdLock();

    if (pageTables.find(procIdx) == pageTables.end()) {
        pageTableLock.upgrade();
        pageTables[procIdx] = PageTable();
        pageTableLock.downgrade();
    }

    updateProcMaps();
    /*
    Address startPageNum = jvmHeapBase >> ilog2(pageSize);
    Address endPageNum = (jvmHeapBase + usage) >> ilog2(pageSize);

    auto& pageTable = pageTables[procIdx];
    for (Address p = startPageNum; p <= endPageNum; p++) {
        if (pageTable.find(p) == pageTable.end()) {
            pageTableLock.upgrade();
            Address pPageNum = (procMask >> ilog2(pageSize / zinfo->lineSize)) | p;
            //info("VirtualMemory::setJvmHeapUsage add page %lx", p << ilog2(pageSize));
            pageTable[p] = pPageNum;
            pageTableLock.downgrade();
        }
    }
    */
    pageTableLock.rdUnlock();
    info("VirtualMemory::setJvmHeapUsage after: memoryfootprint = %lu", getMemoryFootprint());
};

void VirtualMemory::updateProcMaps() {
    FILE* fp;
    char mapbuf[2048];
    auto pid = getpid();
    char filename[1024];
    sprintf(filename, "/proc/%u/maps", pid);
    info("%s %d\n", filename, procIdx);

    fp = fopen(filename, "r");
    assert(fp != NULL);
    uint64_t mappedFootprint = 0;
    auto& pageTable = pageTables[procIdx];

    do {
        uint64_t start, end;
        char flags[1024];
        uint64_t file_offset, inode;
        unsigned int dev_major, dev_minor;
        char mmapfile[1024];

        char* res = fgets(mapbuf, sizeof(mapbuf), fp);
        if (0) info("%s\n", res);

        mmapfile[0] = '\0'; // reset this because there are some entries with empty mmapfile column
        sscanf(mapbuf,"%lx-%lx %s %lx %x:%x %lu %s", &start, &end, flags, &file_offset, &dev_major, &dev_minor, &inode, mmapfile);

        // check for mmapped files in /run/shm/ folder
        //if (strstr(mmapfile, "/run/shm") != NULL) {
        //if (inode != 0 && strstr(flags, "r") != nullptr) { // poan: track all inode we can read in the /proc/maps
        if (strstr(flags, "r") != nullptr) { // poan: track all inode we can read in the /proc/maps
            if (strstr(mmapfile, "SYSV") != nullptr) continue; 
            auto size = end - start;
            info("size %ld KB: %lx-%lx %s %lx %x:%x %lu %s\n", size >> 10, start, end, flags, file_offset, dev_major, dev_minor, inode, mmapfile);
            //info("New inode: %d %lu %s\n", inodeIdx, inode, mmapfile);
            mappedFootprint += size;
            uint64_t startPageNum = start >> ilog2(pageSize);
            uint64_t endPageNum = end >> ilog2(pageSize); 
            for (Address p = startPageNum; p <= endPageNum; p++) {
                if (pageTable.find(p) == pageTable.end()) {
                    pageTableLock.upgrade();
                    Address pPageNum = (procMask >> ilog2(pageSize / zinfo->lineSize)) | p;
                    pageTable[p] = pPageNum;
                    pageTableLock.downgrade();
                }
            }
        }
    } while(!feof(fp));

    info("mappedFootprint %ld MB", mappedFootprint >> 20);
    fclose(fp);

    return;
}


