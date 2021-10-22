#include <sstream>

#include "bithacks.h"
#include "lcp_mem.h"
#include "log.h"
#include "pin.H"

extern unsigned GeneralCompress(char * buffer, unsigned _blockSize, unsigned compress);

uint64_t getCompressedMemoryFootprintGlob() {
    LcpVirtualMemory* lcpVm = dynamic_cast<LcpVirtualMemory*>(zinfo->vm);
    return lcpVm->getCompressedMemoryFootprint();
}

LcpVirtualMemory::LcpVirtualMemory(AggregateStat* parentStat, Config& config) :
    VirtualMemory(parentStat, config) {
    AggregateStat* s = new AggregateStat();
    s->init("lcp", "lcp stats");
    profCompressedMemoryFootprint.init("compressedfootprint", "Compressed memory footprint", &getCompressedMemoryFootprintGlob);
    s->append(&profCompressedMemoryFootprint);
    profTypeOneOverflow.init("typeOneOverflow", "# of type 1 overflow");
    s->append(&profTypeOneOverflow);
    profTypeTwoOverflow.init("typeTwoOverflow", "# of type 2 overflow");
    s->append(&profTypeTwoOverflow);

    parentStat->append(s);
}

uint64_t LcpVirtualMemory::translate(Address vLineAddr, Address& pLineAddr) {
    Address vPageNum = vLineAddr >> ilog2(pageSize / zinfo->lineSize);
    pageTableLock.rdLock();
    if (lcpPageTables.find(procIdx) == lcpPageTables.end()) {
        pageTableLock.upgrade();
        lcpPageTables[procIdx] = LcpPageTable();
        pageTableLock.downgrade();
    }
    auto& lcpPageTable = lcpPageTables[procIdx];
    if (lcpPageTable.find(vPageNum) == lcpPageTable.end()) {
        pageTableLock.upgrade();
        compressPage(vPageNum);
        pageTableLock.downgrade();
    }
    pageTableLock.rdUnlock();
    //info("Lcp: translate vLineAddr %lx", vLineAddr);
    return VirtualMemory::translate(vLineAddr, pLineAddr);
}

uint64_t LcpVirtualMemory::getCompressedMemoryFootprint() {
    uint64_t compressedFootprint = 0;
    //info("getCompressedMemoryFootprint()");
    for (auto ptb : lcpPageTables) {
        for (auto pte : ptb.second) {
            //info("Page %lx, size %ld, totalSize %ld", pte.first, pte.second.pageSize, compressedFootprint);
            compressedFootprint += pte.second.pageSize;
        }
    }
    return compressedFootprint;
}

void LcpVirtualMemory::setJvmHeapUsage(uint64_t usage) {
    VirtualMemory::setJvmHeapUsage(usage);
    if (jvmHeapBase == 0) return; // the base is not yet set;
    
    info("LcpMemory::setJvmHeapUsage before: memoryfootprint = %lu", getCompressedMemoryFootprint());
    pageTableLock.rdLock();
    if (lcpPageTables.find(procIdx) == lcpPageTables.end()) {
        pageTableLock.upgrade();
        lcpPageTables[procIdx] = LcpPageTable();
        pageTableLock.downgrade();
    }

    auto& lcpPageTable = lcpPageTables[procIdx];
    auto pageTable = pageTables[procIdx];
    for (auto p : pageTable) {
        if (lcpPageTable.find(p.first) == lcpPageTable.end()) {
            pageTableLock.upgrade();
            compressPage(p.first);
            pageTableLock.downgrade();
        }
    }
    /*
    Address startPageNum = jvmHeapBase >> ilog2(pageSize);
    Address endPageNum = (jvmHeapBase + usage) >> ilog2(pageSize);
    for (Address p = startPageNum; p <= endPageNum; p++) {
        if (lcpPageTable.find(p) == lcpPageTable.end()) {
            pageTableLock.upgrade();
            compressPage(p);
            pageTableLock.downgrade();
        }
    }
    */
    pageTableLock.rdUnlock();

    info("LcpMemory::setJvmHeapUsage after: memoryfootprint = %lu", getCompressedMemoryFootprint());
}

void LcpVirtualMemory::compressPage(Address pageNum) {
    auto& lcpPageTable = lcpPageTables[procIdx];
    // 1. find the compressed line sizes
    Address startAddr = pageNum << 12;
    Address endAddr = startAddr + 4096;
    g_vector<uint64_t> cLineSizes;
    for (Address addr = startAddr; addr < endAddr; addr += blockSize) {
        char buffer[ blockSize ];
        size_t ncopied = PIN_SafeCopy(buffer, (void*) addr, blockSize);
        if (ncopied != blockSize) warn("Lcp::compressPage - Only copied %lu bytes (out of %u)", ncopied, blockSize);
        //return BDICompress(buffer, blockSize);
        cLineSizes.push_back(GeneralCompress(buffer, blockSize, 3));
    }
    assert(cLineSizes.size() == pageSize / blockSize);
    // 2. calculate exception storage size and the minimal page size
    uint64_t minPageSize = 4096;
    LcpPTE minPagePTE = {LcpMetaData(), 4096, 0, 64, 0, false};
    for (auto cSize : compressedLineSizes) {
        //uint64_t cPageSize = cSize * 64 + 64; // compressed lines + metadata
        uint64_t cPageSize = cSize * 64; // let's take out the metadata 
        LcpMetaData metaData; 
        uint8_t eidx = 0;
        for (uint32_t i = 0; i < 64; i++) metaData.vbit[eidx] = false;

        // find exceptions
        for (uint32_t i = 0; i < 64; i++) {
            uint64_t cLineSize = cLineSizes[i];
            if (cLineSize > cSize) {
                cPageSize += 64;
                metaData.ebit[i] = true;
                metaData.eindex[i] = eidx;
                metaData.vbit[eidx] = true;
                eidx++;
            } else {
                metaData.ebit[i] = false;
                metaData.eindex[i] = 0;
            }
        }
        if (cPageSize <= minPageSize) {
            minPageSize = cPageSize;
            minPagePTE.metaData = metaData;
            minPagePTE.csize = cSize;
        }
    }

    // 3. find the os page size
    uint64_t osPageSize = 4096;
    for (auto cPageSize : compressedPageSizes) {
        if (minPageSize <= cPageSize && osPageSize > cPageSize) {
            osPageSize = cPageSize;
        }
    }
    minPagePTE.pageSize = osPageSize;
    if (minPagePTE.pageSize < 4096) minPagePTE.cbit = true;

    // 4. update the PTE
    /*
    info("Page %lx, minSize %lu, csize %u, osPageSize %lu", pageNum << 12, minPageSize, minPagePTE.csize, osPageSize);
    for (uint32_t l = 0; l < 64; l++) {
        info("  clineSize: %lu Metadata: l %u ebit %d eindex %d", cLineSizes[l], l, minPagePTE.metaData.ebit[l], minPagePTE.metaData.eindex[l]);
    }
    */
   
    lcpPageTable[pageNum] = minPagePTE;
}

uint64_t LcpVirtualMemory::dirtyWriteBack(Address lineAddr) {
    auto& pageTable = lcpPageTables[procIdx];
    Address pageNum = lineAddr >> ilog2(pageSize / zinfo->lineSize);
    auto& pte = pageTable[pageNum];
    if (!pte.cbit) return 0; // not a compressed page, whatever

    auto& metaData = pte.metaData;
    uint64_t pageMask = (1 << (12 - ilog2(zinfo->lineSize))) - 1;
    uint64_t lineIdx = lineAddr & pageMask;
    if (metaData.ebit[lineIdx]) return 0; // line was an exception, whatever

    char buffer[ blockSize ];
    size_t ncopied = PIN_SafeCopy(buffer, (void*)(lineAddr << ilog2(blockSize)), blockSize);
    if (ncopied != blockSize) warn("Lcp::dirtyWriteback - Only copied %lu bytes (out of %u)", ncopied, blockSize);

    uint32_t newSize = GeneralCompress(buffer, blockSize, 3);
    //info("Lcp:dirtyWriteback, page %lx line %lx old size %d, new size %d", pageNum, lineAddr, pte.csize, newSize);
    if (newSize <= pte.csize) {
        if (metaData.ebit[lineIdx]) { // this line now fits
            metaData.ebit[lineIdx] = false;
            metaData.vbit[metaData.eindex[lineIdx]] = false;
            metaData.eindex[lineIdx] = 0;
        }
        return 0; // line fits in the slot, whatever
    }
    // search a slot
    uint32_t eStorageSize = pte.pageSize - 64 * pte.csize - 64;
    //info("  eStorageSize %u", eStorageSize);
    for (uint32_t i = 0; i < 64; i++) {
        if ((i + 1) * 64 > eStorageSize) break; // the first invalid exception bit
        else if (!metaData.vbit[i]) { // find a slot
            metaData.ebit[lineIdx] = true;
            metaData.eindex[lineIdx] = i;
            metaData.vbit[i] = true;
            //info("page %lx use slot %d", pageNum, i);
            return 0;
        }
    }

    // no slots, it's an overflow!
    // copy the old one
    LcpPTE oldPte = pte;
    //info("Exception: old size %d new size %d", oldPte.csize, pte.csize);
    compressPage(pageNum);
    pte = pageTable[pageNum]; 
    if (oldPte.csize == pte.csize) { // just need more exception spaces
        profTypeOneOverflow.inc();
        return 20 * 1000;
    } else { // the whole page is recompressed
        profTypeTwoOverflow.inc();
        return 100 * 1000; 
    }
}

uint32_t LcpVirtualMemory::getNumOfFreeLines(Address lineAddr) {
    auto& pageTable = lcpPageTables[procIdx];
    Address pageNum = lineAddr >> (12 - ilog2(zinfo->lineSize));
    auto& pte = pageTable[pageNum];
    auto& metaData = pte.metaData;
    int32_t csize = pte.csize;
    //info("          PageNum: %lx pagesize %ld csize %d", pageNum, pte.pageSize, csize);
    //for (uint32_t l = 0; l < 64; l++) {
    //    info("              Metadata: l %d ebit %d eindex %d", l, metaData.ebit[l], metaData.eindex[l]);
    //}
    Address lineMask = (1 << (12 - ilog2(zinfo->lineSize))) - 1;
    Address lineOffset = lineAddr & lineMask;
    assert(lineOffset < 64);
    uint32_t nfreeLines = 0;
    for (int32_t fetchSize = 64 - csize; fetchSize >= csize; fetchSize -= csize) {
        lineOffset++;
        //info("          %d %d lineOffset %lu ebit %d eindex %d", fetchSize, csize, lineOffset, metaData.ebit[lineOffset], metaData.eindex[lineOffset]);
        if (lineOffset >= 64) break;
        if (!metaData.ebit[lineOffset]) { // if this is not exception
            nfreeLines++;
            //info("          freeLine++: %d", nfreeLines);
        }
    }
    assert(nfreeLines < 64);
    return nfreeLines;
}
