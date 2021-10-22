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

#ifndef SRC_MEMORY_ACCESS_PREDICTOR_H_
#define SRC_MEMORY_ACCESS_PREDICTOR_H_

#include "g_std/g_string.h"
#include "galloc.h"
#include "hash.h"
#include "memory_hierarchy.h"
#include "stats.h"
#include "zsim.h"

/* This class is used for modeling the memory access predictor in "Alloy Cache" (Qurishi et al.
 * MICRO'12). Basically, it's a counter-based predictor indexed by memory address. Two modes
 * are implemented here: global-history-based and instuction-based, where the later should
 * perform better.
 */

class MemoryAccessPredictor : public GlobAlloc {
    private:
        g_string name;

        bool insBased;
        uint32_t counterLength;
        uint32_t tableIdxLength;
        uint32_t numEntries;
        uint32_t* table;
        HashFamily* hashFunc;

        uint64_t FNVHash(uint64_t* input) {  // a folded XOR hashing
            uint64_t hash = 14695981039346656037ULL;
            uint64_t FNV_prime = 1099511628211ULL;
            char* octet = (char*) input;
            for (uint32_t i = 0; i < 8; i++) {
                hash *= FNV_prime;
                hash ^= *octet;
                octet++;
            }
            return hash;
        }
        // Performance Stats
        Counter profHH, profHM, profMH, profMM, profCorrect, profIncorrect;
        VectorCounter profCoreHH, profCoreHM, profCoreMH, profCoreMM, profCoreCorrect, profCoreIncorrect;

    public:
        MemoryAccessPredictor(const char* _name, bool _insBased, uint32_t _counterLength,
                uint32_t _tableIdxLength) : name(_name), insBased(_insBased),
                counterLength(_counterLength), tableIdxLength(_tableIdxLength),
                hashFunc(new H3HashFamily(1, _tableIdxLength, 0x7AD07ADD))
        {
            uint32_t ncore = zinfo->numCores;
            numEntries = 1 << tableIdxLength;
            table = gm_calloc<uint32_t>(ncore*numEntries);
            for (uint32_t c = 0; c < ncore; c++) {
                for (uint32_t idx = 0; idx < numEntries; idx++) {
                    table[ c*numEntries + idx] = (1 << (counterLength-1)) - 1;  // weak miss
                }
            }
        }

        void reset() {
            uint32_t ncore = zinfo->numCores;
            numEntries = 1 << tableIdxLength;
            for (uint32_t c = 0; c < ncore; c++) {
                for (uint32_t idx = 0; idx < numEntries; idx++) {
                    table[ c*numEntries + idx] = (1 << (counterLength-1)) - 1;  // weak miss
                }
            }
        }

        void sat_inc(uint32_t idx, uint32_t core) {
            if (table[core*numEntries+idx] + 1 < (uint32_t)(1 << counterLength)) {
                table[core*numEntries+idx]++;
            }
        }

        void sat_dec(uint32_t idx, uint32_t core) {
            if (table[core*numEntries+idx] > 0) {
                table[core*numEntries+idx]--;
            }
        }
        void initStats(AggregateStat* parentStat) {
            AggregateStat* predictorStat = new AggregateStat();
            predictorStat->init(name.c_str(), "Predictor Stats");
            auto initCounter = [predictorStat] (Counter& c, const char* shortDesc, const char* longDesc) {
                c.init(shortDesc, longDesc);
                predictorStat->append(&c);
            };
            auto initCoreCounter = [predictorStat] (VectorCounter& vc, const char* shortDesc, const char* longDesc, uint32_t vectorSize) {
                vc.init(shortDesc, longDesc, vectorSize);
                predictorStat->append(&vc);
            };

            initCounter(profHH, "HH", "Predict Hit, actual Hit");
            initCounter(profHM, "HM", "Predict Hit, actual Miss");
            initCounter(profMH, "MH", "Predict Miss, actual Hit");
            initCounter(profMM, "MM", "Predict Miss, actual Miss");
            initCounter(profCorrect, "Correct", "Predict correctly");
            initCounter(profIncorrect, "Incorrect", "Predict incorrectly");

            initCoreCounter(profCoreHH, "CoreHH", "Predict Hit, actual Hit per Core", zinfo->numCores);
            initCoreCounter(profCoreHM, "CoreHM", "Predict Hit, actual Miss per Core", zinfo->numCores);
            initCoreCounter(profCoreMH, "CoreMH", "Predict Miss, actual Hit per Core", zinfo->numCores);
            initCoreCounter(profCoreMM, "CoreMM", "Predict Miss, actual Miss per Core", zinfo->numCores);
            initCoreCounter(profCoreCorrect, "CoreCorrect", "Predict correctly per Core", zinfo->numCores);
            initCoreCounter(profCoreIncorrect, "CoreIncorrect", "Predict incorrectly per Core", zinfo->numCores);

            parentStat->append(predictorStat);
        }
        void update(Address pc, bool isMiss, uint32_t core) {
            // for global-history-based, only use one counter(table[0])
            uint32_t hashPC = FNVHash(&pc) % (1 << tableIdxLength);  // FNV may or may not be the same
            // uint32_t hashPC = hashFunc->hash(0, pc);
            uint32_t idx = insBased? hashPC : 0;
            bool predictMiss = predictIsMiss(pc, core);

            // Update Stats
            if (predictMiss == isMiss) {
                profCorrect.inc();
                profCoreCorrect.inc(core);
                if (predictMiss) {
                    profMM.inc();
                    profCoreMM.inc(core);
                } else {
                    profHH.inc();
                    profCoreHH.inc(core);
                }
            } else {
                profIncorrect.inc();
                profCoreIncorrect.inc(core);
                if (predictMiss) {
                    profMH.inc();
                    profCoreMH.inc(core);
                } else {
                    profHM.inc();
                    profCoreHM.inc(core);
                }
            }

            // info("Updating: %lu, idx: %d, counter: %d", pc, idx, table[core*numEntries + idx]);
            if (isMiss) {
                sat_dec(idx, core);
            } else {
                sat_inc(idx, core);
            }

            // info("Updated: %lu, idx: %d, counter: %d", pc, idx, table[core*numEntries + idx]);
        }

        bool predictIsMiss(Address pc, uint32_t core) {
            uint32_t hashPC = FNVHash(&pc) % (1 << tableIdxLength);
            // uint32_t hashPC = hashFunc->hash(0, pc);
            uint32_t idx = insBased? hashPC : 0;
            // info("Predicting: %lu, idx: %d, counter: %d", pc, idx, table[core*numEntries + idx]);
            return table[core*numEntries+idx] < (uint32_t)(1 << (counterLength-1) );
        }

        uint64_t getMMCounts() const {return profMM.get();}
        uint64_t getHMCounts() const {return profHM.get();}
        uint64_t getMHCounts() const {return profMH.get();}
        uint64_t getHHCounts() const {return profHH.get();}
        uint64_t getTotalCounts() const {return profCorrect.get() + profIncorrect.get();}

        uint64_t getCoreMMCounts(uint32_t core) const {return profCoreMM.count(core);}
        uint64_t getCoreHMCounts(uint32_t core) const {return profCoreHM.count(core);}
        uint64_t getCoreMHCounts(uint32_t core) const {return profCoreMH.count(core);}
        uint64_t getCoreHHCounts(uint32_t core) const {return profCoreHH.count(core);}
        uint64_t getCoreTotalCounts(uint32_t core) const 
        {return profCoreCorrect.count(core) + profCoreIncorrect.count(core);}
};

#endif  // SRC_MEMORY_ACCESS_PREDICTOR_H_
