#include "compressed_cache_array.h"
#include "repl_policies.h"
#include "hash.h"

namespace compressed {

//const uint32_t DEBUG_SET = 222; // 1201244
const uint32_t DEBUG_SET = -1; // 1201244

CacheArray::CacheArray(
    uint32_t _numBlocks,
    uint32_t _associativity,
    HashFamily* _hf,
    uint32_t _extraTagRatio)
        : blockSize(zinfo->lineSize)
        , associativity(_associativity)
        , rp(0)
        , hf(_hf)
        , extraTagRatio(_extraTagRatio) {

    numSets = _numBlocks / _associativity;
    setSizeInBytes = blockSize * _associativity;

    setAvailableSpace = gm_calloc<uint32_t>(numSets);
    for (uint32_t s = 0; s < numSets; s++) {
        setAvailableSpace[s] = setSizeInBytes;
    }

    numEntries = _numBlocks * blockSize;
    entries = gm_calloc<Entry>(numEntries);
    for (uint32_t e = 0; e < numEntries; e++) {
        entries[e].lineAddr = 0;
        entries[e].size = 0;
    }

    info("compressed::CacheArray - Constructed compressed cache with %u sets and %u entries",
         numSets, numEntries);
}

CacheArray::~CacheArray() {
    gm_free(setAvailableSpace);
    gm_free(entries);
}

// look for the address, but only return a hit if its size hasn't changed.
// otherwise, its effectively a miss, so remove the entry if it exists.
int32_t CacheArray::lookup(const Address lineAddr, const MemReq* req, bool updateReplacement, bool fullyInvalidate) {
    uint32_t set = hf->hash(0, lineAddr) % numSets;
    if (set == DEBUG_SET) checkpoint();
    if (set == DEBUG_SET) info("%lx %u %d", lineAddr << 6, set, numSets);

    
    uint32_t start = set * setSizeInBytes;
    uint32_t end = start + min(setSizeInBytes, associativity * extraTagRatio);

    for (uint32_t id = start; id < end; id++) {
        if (entries[id].lineAddr == lineAddr) {
            // if size of the line changes on a write, we may be
            // forced to evict lines to make space
            if (!IsGet(req->type)) {
                uint32_t compressedSize = getCompressedSizeFromAddr(req->lineAddr);
                // bigger lines need space: make space for the line, starting by evicting it
                if (compressedSize > entries[id].size) {
                    entries[id].lineAddr = 0; // this makes sure the replacement policy won't try to evict id
                    setAvailableSpace[set] += entries[id].size;
                    makeSpace(set, compressedSize, req);
                    assert(setAvailableSpace[set] >= compressedSize);
                    
                    entries[id].lineAddr = lineAddr; // re-allocate line
                    entries[id].size = compressedSize;
                    setAvailableSpace[set] -= compressedSize;
                    if (set == DEBUG_SET) info("compressed::CacheArray::lookup - Grabbed space - line %d, addr 0x%lx, size %u, available %u, set %u",
                         id, entries[id].lineAddr << 6, entries[id].size, setAvailableSpace[set], set);
                }
                // smaller lines free space
                else if (compressedSize < entries[id].size) {
                    setAvailableSpace[set] += entries[id].size - compressedSize;
                    entries[id].size = compressedSize;
                    if (set == DEBUG_SET) info("compressed::CacheArray::lookup - Freed space - line %d, addr 0x%lx, size %u, available %u, set %u",
                         id, entries[id].lineAddr << 6, entries[id].size, setAvailableSpace[set], set);
                }
            }
            
            if (updateReplacement) { rp->update(id, req); }
            if (set == DEBUG_SET) checkpoint();
            return id;
        }
    }
    if (set == DEBUG_SET) checkpoint();
    return -1;
}

void CacheArray::makeSpace(uint32_t set, uint32_t requiredSpace, const MemReq* req) {
    if (set == DEBUG_SET) info("compressed::CacheArray::makeSpace set %u, availableSpace %u, requiredSpace %u",
         set, setAvailableSpace[set], requiredSpace);

    // trigger evictions until space is free!
    // poan: or at least a tag is available
    uint32_t startTag = set * setSizeInBytes;
    uint32_t endTag = startTag + associativity * extraTagRatio;
    bool forceEviction = true;
    for (uint32_t i = startTag; i < endTag; i++) {
        if (entries[i].lineAddr == 0) {
            forceEviction = false;
            break;
        }
    }
    while (setAvailableSpace[set] < requiredSpace || forceEviction) {
        auto cands = Cands(this, set * setSizeInBytes, set * setSizeInBytes + associativity * extraTagRatio);
        //assert(setSizeInBytes - cands.getTotalCandidateSize() == setAvailableSpace[set]);

        int32_t victim = rp->rankCands(
            (const MemReq*) 0, // why does rankCands take a MemReq ?!
            cands);
        assert(victim != -1);
        // TODO: model invalidation here!
        rp->replaced(victim);
        if (set == DEBUG_SET) info("compressed::CacheArray::processEviction lineId %d, lineAddr %lx", victim, entries[victim].lineAddr << 6);
        cc->processEviction(*req, entries[victim].lineAddr, victim, zinfo->numPhases);

        if (set == DEBUG_SET) info("compressed::CacheArray::makeSpace - Evicted - line %d, addr 0x%lx, size %u, available %u, set %u",
             victim, entries[victim].lineAddr << 6, entries[victim].size, setAvailableSpace[set], set);
        entries[victim].lineAddr = 0;

        // we evict something
        forceEviction = false;

        setAvailableSpace[set] += entries[victim].size;
        assert(entries[victim].size > 0);

        // fixme: this doesn't model writebacks!!!!
    }

    if (set == DEBUG_SET) checkpoint();
}

uint32_t CacheArray::preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr) {
    uint32_t set = hf->hash(0, lineAddr) & (numSets - 1);

    makeSpace(set, getCompressedSizeFromAddr(req->lineAddr), req);
    *wbLineAddr = 0;                    // only works beyond coherence!

    // now find empty entry
    uint32_t first = set * setSizeInBytes;
    for (uint32_t id = first; id < first + min(setSizeInBytes, associativity * extraTagRatio); id++) {
        if (entries[id].lineAddr == 0) {
            return id;
        }
    }
    panic("Should never reach here.");
}

void CacheArray::postinsert(const Address lineAddr, const MemReq* req, uint32_t lineId) {
    //checkpoint();
    
    uint32_t set = lineId / setSizeInBytes; // hf->hash(0, lineAddr) % numSets;

    uint32_t compressedSize = getCompressedSizeFromAddr(req->lineAddr);
    entries[lineId].lineAddr = lineAddr;
    entries[lineId].size = compressedSize;
    setAvailableSpace[set] -= compressedSize;
    rp->update(lineId, req);

    if (set == DEBUG_SET) info("compressed::CacheArray::postInsert - Allocated - line %d, addr 0x%lx, size %u, available %u, set %u",
         lineId, entries[lineId].lineAddr << 6, entries[lineId].size, setAvailableSpace[set], set);
    if (set == DEBUG_SET) {
        uint32_t startTag = set * setSizeInBytes;
        uint32_t endTag = startTag + associativity * extraTagRatio;
        for (uint32_t i = startTag; i < endTag; i++) {
            info("  Tag: %d addr: %lx size %d", i, entries[i].lineAddr << 6, entries[i].size);
        }
    }
}

// CompressedCacheReplPolicy::CompressedCacheReplPolicy(uint32_t _numLines)
//         : blockSizes(0) {
//     blockSizes = gm_calloc<uint32_t>(_numLines);
// }

// CompressedCacheReplPolicy::~CompressedCacheReplPolicy() {
//     gm_free(blockSizes);
//     blockSizes = 0;
// }

// void CompressedCacheReplPolicy::update(uint32_t id, const MemReq* req) {
//     uint32_t blockSize = 
// }

// void CompressedCacheReplPolicy::replaced(uint32_t id, const MemReq* req) {

// }

} // namespace compressed
