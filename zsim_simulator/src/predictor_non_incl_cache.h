#pragma once

#include "non_incl_cache.h"
#include "memory_access_predictor.h"

class PredictorNonInclusiveCache : public NonInclusiveCache {
    private:
        MemoryAccessPredictor* predictor;

    public:
        PredictorNonInclusiveCache(CC* cc, CacheArray* _array,
                ReplPolicy* _rp, uint32_t _tagLat, uint32_t _dataLat, g_string& _name,
                MemoryAccessPredictor* _predictor)
            : NonInclusiveCache(cc, _array, _rp, _tagLat, _dataLat, _name)
            , predictor(_predictor)
        {}

        uint64_t doTagLookup(Address lineAddr, uint64_t cycle, AccessType type, uint32_t srcId) {
            Address pc = zinfo->cores[srcId]->getCurPC();
            bool predictMiss = predictor->predictIsMiss(pc, srcId) && type == GETS;
            bool isMiss = probe(lineAddr) == -1;
            if (type == GETS) {
                predictor->update(pc, isMiss, srcId);
            }

            if (predictMiss and isMiss) {
                return cycle;
            } else if (predictMiss and !isMiss) {
                // do a fake access
                MESIState dummyState = I;
                MemReq parentReq = {lineAddr, type, getChildId(), &dummyState, cycle,
                                        NULL, dummyState, srcId, 0};
                (getParent())->access(parentReq);
                return cycle + getTagLat();
            } else {
                return cycle + getTagLat();
            }
        
        }

        void initCacheStats(AggregateStat* cacheStat) {
            NonInclusiveCache::initCacheStats(cacheStat);
            predictor->initStats(cacheStat);
        }

};
