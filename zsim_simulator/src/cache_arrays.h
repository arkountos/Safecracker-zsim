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

#ifndef CACHE_ARRAYS_H_
#define CACHE_ARRAYS_H_

#include "memory_hierarchy.h"
#include "stats.h"

class ReplPolicy;
class HashFamily;

/* General interface of a cache array. The array is a fixed-size associative container that
 * translates addresses to line IDs. A line ID represents the position of the tag. The other
 * cache components store tag data in non-associative arrays indexed by line ID.
 */
class CacheArray : public GlobAlloc {
    public:
        /* Returns tag's ID if present, -1 otherwise. If updateReplacement is set, call the replacement policy's update() on the line accessed.
         * If fullyInvalidate is true lookup is done for full invalidation purposes.
         */
        virtual int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement, bool fullyInvalidate) = 0;

        /* Runs replacement scheme, returns tag ID of new pos and address of line to write back*/
        virtual uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr) = 0;

        /* Actually do the replacement, writing the new address in lineId.
         * NOTE: This method is guaranteed to be called after preinsert, although
         * there may be some intervening calls to lookup. The implementation is
         * allowed to keep internal state in preinsert() and use it in postinsert()
         */
        virtual void postinsert(const Address lineAddr, const MemReq* req, uint32_t lineId) = 0;

        virtual void initStats(AggregateStat* parent) {};

        virtual void clear() = 0; // clear the whole tag array
#ifdef CLU_STATS_ENABLED
        /* Process access cache line utilization statistics. */
        virtual void processAccessCLUStats(const MemReq& req, const uint32_t lineIndex) {};
#endif

        /* The maximum number of lines in this cache array.  E.g.,
         * compressed caches will return a different value from a
         * set-associative cache.
         */
        virtual uint32_t getNumLines() const = 0;

        /* Resolve initialization cycles.
         */
        virtual void init(ReplPolicy* _rp) = 0;

};
/* Set-associative cache array */
class SetAssocArray : public CacheArray {
    protected:
        Address* array;
#ifdef CLU_STATS_ENABLED
        volatile CacheLineAccessMask_t * accessMask; // cache lines' access masks
        Counter countUCLC; // utilized cache line chunks counter
#endif
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;

    public:
        SetAssocArray(uint32_t _numLines, uint32_t _assoc, HashFamily* _hf);

        int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement, bool fullyInvalidate);
        uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr);
        void postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate);
        void initStats(AggregateStat* parentStat);
        void clear();
#ifdef CLU_STATS_ENABLED
        void processAccessCLUStats(const MemReq& req, const uint32_t lineIndex);
#endif
        uint32_t getNumLines() const { return numLines; }
        void init(ReplPolicy* _rp) { rp = _rp; }
};

/* The cache array that started this simulator :) */
class ZArray : public CacheArray {
    private:
        Address* array; //maps line id to address
        uint32_t* lookupArray; //maps physical position to lineId
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t ways;
        uint32_t cands;
        uint32_t setMask;

        //preinsert() stores the swaps that must be done here, postinsert() does the swaps
        uint32_t* swapArray; //contains physical positions
        uint32_t swapArrayLen; //set in preinsert()

        uint32_t lastCandIdx;

        Counter statSwaps;

    public:
        ZArray(uint32_t _numLines, uint32_t _ways, uint32_t _candidates, HashFamily* _hf);

        int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement, bool fullyInvalidate);
        uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr);
        void postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate);
        void clear() {panic("Not implemented");}
        //zcache-specific, since timing code needs to know the number of swaps, and these depend on idx
        //Should be called after preinsert(). Allows intervening lookups
        uint32_t getLastCandIdx() const {return lastCandIdx;}

        void initStats(AggregateStat* parentStat);
        uint32_t getNumLines() const { return numLines; }
        void init(ReplPolicy* _rp) { rp = _rp; }

};

// Simple wrapper classes and iterators for candidates in each case; simplifies replacement policy interface without sacrificing performance
// NOTE: All must implement the same interface and be POD (we pass them by value)
struct SetAssocCands {
    struct iterator : public std::iterator<std::bidirectional_iterator_tag, int32_t> {
        uint32_t x;
        explicit inline iterator(uint32_t _x) : x(_x) {}
        inline void inc() {x++;} //overloading prefix/postfix too messy
        inline iterator& operator++() { x++; return *this; }
        inline iterator& operator++(int) { return ++(*this); }
        inline iterator& operator+=(uint32_t offset) { x += offset; return *this; }
        inline uint32_t operator*() const { return x; }
        inline bool operator==(const iterator& it) const { return it.x == x; }
        inline bool operator!=(const iterator& it) const { return it.x != x; }
    };

    uint32_t b, e;
    inline SetAssocCands(uint32_t _b, uint32_t _e) : b(_b), e(_e) {}
    inline iterator begin() const {return iterator(b);}
    inline iterator end() const {return iterator(e);}
    inline uint32_t numCands() const { return e-b; }
    inline uint32_t size() const { return e-b; }

};


struct ZWalkInfo {
    uint32_t pos;
    uint32_t lineId;
    int32_t parentIdx;

    inline void set(uint32_t p, uint32_t i, int32_t x) {pos = p; lineId = i; parentIdx = x;}
};

struct ZCands {
    struct iterator : public std::iterator<std::bidirectional_iterator_tag, int32_t> {
        ZWalkInfo* x;
        explicit inline iterator(ZWalkInfo* _x) : x(_x) {}
        inline void inc() {x++;} //overloading prefix/postfix too messy
        inline iterator& operator++() { x++; return *this; }
        inline iterator& operator++(int) { return ++(*this); }
        inline uint32_t operator*() const { return x->lineId; }
        inline bool operator==(const iterator& it) const { return it.x == x; }
        inline bool operator!=(const iterator& it) const { return it.x != x; }
    };

    ZWalkInfo* b;
    ZWalkInfo* e;
    inline ZCands(ZWalkInfo* _b, ZWalkInfo* _e) : b(_b), e(_e) {}
    inline iterator begin() const {return iterator(b);}
    inline iterator end() const {return iterator(e);}
    inline uint32_t numCands() const { return e-b; }
    inline uint32_t size() const { return e-b; }
};

#endif  // CACHE_ARRAYS_H_
