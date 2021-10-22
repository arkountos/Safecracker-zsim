#pragma once

#include "compressed_cache_array.h"
#include "repl_policies.h"
#include "rrip_repl_policies.h"
#include "bithacks.h"

namespace compressed {

class ReplPolicy
        : public virtual ::ReplPolicy {
    public:
        ReplPolicy(compressed::CacheArray* _carray) : carray(_carray) {}
        virtual ~ReplPolicy() {}

        // base repl policy interface -- do NOT override!
        virtual void update(uint32_t id, const MemReq* req) = 0;
        virtual void replaced(uint32_t id) = 0;

    protected:
        compressed::CacheArray* carray;
};

class CAMPReplPolicy
        : public compressed::ReplPolicy
        , public BRRIPReplPolicy {
    public:

        CAMPReplPolicy(
            compressed::CacheArray* _carray,
            uint32_t _numLines,
            uint32_t M,
            uint32_t _samplingFactor)
                : compressed::ReplPolicy(_carray)
                , BRRIPReplPolicy(_numLines, M)
                , sampledLines(numLines / _samplingFactor)
                , debugOutputCounter(0) {

            assert(zinfo->lineSize == (1 << LOG_LINE_SIZE));     // need to change constants if this changes
        }

        ~CAMPReplPolicy() {
        }

        void update(uint32_t id, const MemReq* req) {
            // find size bucket (log scale)
            uint32_t size = carray->getCompressedSizeFromId(id);
            uint32_t logSize = ilog2(size);
            if (size > (1u << logSize)) { ++logSize; }
            assert(logSize <= LOG_LINE_SIZE);

            // debugging
            if (++debugOutputCounter % DEBUG_OUTPUT_INTERVAL == 0) {
                std::stringstream ss;
                ss << "| psel ";
                for (uint32_t i = 0; i <= LOG_LINE_SIZE; i++) {
                    ss << pselCounter[i] << " ";
                }
                uint32_t totalHits = 0;
                ss << "| hits ";
                for (uint32_t i = 0; i <= LOG_LINE_SIZE; i++) {
                    ss << debugHitCounters[i] << " ";
                    totalHits += debugHitCounters[i];
                    debugHitCounters[i] = 0;
                }
                info("CAMP %s | hit rate %.4g", ss.str().c_str(), 1. * totalHits / DEBUG_OUTPUT_INTERVAL);
            }
            if (array[id] != 0) {       // hit
                debugHitCounters[logSize]++;
            }

            // replacement
            if (sampledLines * 2*logSize <= id &&
                id < sampledLines * (2*logSize+1)) {
                // SRRIP
                if (array[id] == 0) {
                    // miss
                    pselCounter[logSize]--;
                } else {
                    pselCounter[logSize]++;
                }
                SRRIPReplPolicy::update(id, req);
            } else if (sampledLines * (2*logSize+1) <= id &&
                       id < sampledLines * (2*logSize+2)) {
                // monitor BRRIP
                if (array[id] == 0) {
                    // miss
                    pselCounter[logSize]++;
                } else {
                    pselCounter[logSize]--;
                }
                BRRIPReplPolicy::update(id, req);
            } else {
                // rest of cache
                if (pselCounter[logSize] >= 0) {
                    SRRIPReplPolicy::update(id, req);
                } else {
                    BRRIPReplPolicy::update(id, req);
                }
            }

            pselCounter[logSize] = MIN(MAX(pselCounter[logSize], -1024), 1023);
        }

        void replaced(uint32_t id) {
            BRRIPReplPolicy::replaced(id);
        }

        DECL_RANK_BINDINGS;

    private:
        uint32_t prioToRank(uint32_t id, uint32_t prio) {
            // return SRRIPReplPolicy::prioToRank(id, prio);
            uint32_t size = carray->getCompressedSizeFromId(id);
            uint32_t rank = (prio << LOG_LINE_SIZE) / size;
            return rank;
        }
        
        static constexpr uint32_t LOG_LINE_SIZE = 6;
        
        uint32_t sampledLines;
        int32_t pselCounter[LOG_LINE_SIZE+1];
        
        static constexpr uint32_t DEBUG_OUTPUT_INTERVAL = 100000;
        uint32_t debugOutputCounter;
        uint32_t debugHitCounters[LOG_LINE_SIZE+1];
};

} // namespace compressed
