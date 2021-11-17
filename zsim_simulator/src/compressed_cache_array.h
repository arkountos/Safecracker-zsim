#pragma once

#include "coherence_ctrls.h"
#include "cache_arrays.h"
#include "zsim.h"
#include "pin.H"

#include <iostream>
#include <iomanip>

extern unsigned BDICompress(char * buffer, unsigned _blockSize);
extern unsigned GeneralCompress(char * buffer, unsigned _blockSize, unsigned compress);

namespace compressed {

class Cands;

class CacheArray
        : public ::CacheArray {
    public:
        CacheArray(uint32_t _numBlocks, uint32_t _associativity, HashFamily* _hf, uint32_t _extraTagRatio);
        virtual ~CacheArray();

        int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement, bool fullyInvalidate);
        uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr);
        void postinsert(const Address lineAddr, const MemReq* req, uint32_t lineId);

	//------
	// We also add initStats
	void initStats(AggregateStat* parent);
	//

        uint32_t getNumLines() const { return numEntries; }
        void init(::ReplPolicy* _rp) { rp = _rp; }
        void clear() { panic("not implemented"); }
        inline uint32_t getCompressedSizeFromAddr(Address addr) {
            // return blockSize;
            char buffer[ blockSize ];
            PIN_SafeCopy(buffer, (void*)(addr << lineBits), blockSize);
            //size_t ncopied = PIN_SafeCopy(buffer, (void*)(addr << lineBits), blockSize);
            //if (ncopied != blockSize) warn("compressed::CacheArray - Only copied %lu bytes (out of %u)", ncopied, blockSize);
            uint32_t size = BDICompress(buffer, blockSize);
	    //compressionCalls++;
            //info("BDICompress: addr %lx size %u", addr << 6, size);
            return size;
            //return BDICompress(buffer, blockSize);
            //return GeneralCompress(buffer, blockSize, 3);
        }

        inline uint32_t getCompressedSizeFromId(uint32_t id) {
            assert(entries[id].lineAddr != 0);
            return entries[id].size;
        }
        void setCC(CC* _cc) {cc = dynamic_cast<MESICC*>(_cc); assert(cc);}

    private:
        void makeSpace(uint32_t set, uint32_t requiredSpace, const MemReq* req);
        struct Entry {
                Address lineAddr;
                uint32_t size;
        };
	
	// -----
	// My stat to count compression calls
	Counter compressionCalls;
	// -----

        uint32_t blockSize;
        uint32_t associativity;
        uint32_t numEntries;
        uint32_t numSets;
        uint32_t setSizeInBytes;
        uint32_t* setAvailableSpace;
        Entry* entries;
        ::ReplPolicy* rp;
        HashFamily* hf;

        // poan: having the cc to invalidate lines that got evicted.
        // Compress cache can evict multiple lines at a time, or none.
        // Therefore, the original preinsert-and-processEviction model
        // won't work here.
        MESICC* cc;

        // poan: this try to model a more realistic compressed cache, where the number of tags
        // is somehow limited. The original implementation has number of entries = bytes,
        // which means 64X more tags for 64B cache lines.
        uint32_t extraTagRatio;

        friend class compressed::Cands;
};

// iterate over valid entries in the compressed cache
struct Cands {
        typedef std::vector<int32_t> container;
        typedef container::const_iterator iterator;

        container cands;
        uint32_t totalCandidateSize;
        uint32_t x;

        inline Cands(CacheArray* _ca, int32_t _b, int32_t _e) {
            totalCandidateSize = 0;
            for (int32_t x = _b; x < _e; x++) {
                if (_ca->entries[x].lineAddr != 0) {
                    cands.push_back(x);
                    totalCandidateSize += _ca->entries[x].size;
                }
            }
        }
        inline iterator begin() const { return cands.begin(); }
        inline iterator end() const { return cands.end(); }
        inline void inc() {x++;}
        inline uint32_t size() const { return cands.size(); }
        inline uint32_t getTotalCandidateSize() const { return totalCandidateSize; }
        inline bool operator==(const iterator& it) const { return it == (cands.begin() + x); }
        inline bool operator!=(const iterator& it) const { return it != (cands.begin() + x); }
};

} // namespace compressed
