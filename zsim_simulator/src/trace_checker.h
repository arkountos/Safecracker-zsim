/* A ManagedMemory that simulates a simple bump pointer allocation with unlimited
 * capacity. It also reports Load/Store/RefUpdate to memory locations that are
 * not initialized via Alloc, i.e., operations that we should be able to
 * capture and simulate but cannot because of various reasons (uncompleted
 * traces, trace generator bugs, race conditions, operations to VM data, and etc.).
 */

#pragma once

#include "g_std/g_unordered_map.h"
#include "managed_memory.h"
#include "stats.h"

class TraceChecker : public ManagedMemory {
    public:
        TraceChecker(GCTraceReader* _gctr, AggregateStat* parentStat) : ManagedMemory(_gctr, parentStat) {}

        void simLoad(Address base, uint32_t tid, int32_t offset) {
            ManagedMemory::simLoad(base, tid, offset);
        }

        void simStore(Address base, uint32_t tid, int32_t offset) {
            ManagedMemory::simStore(base, tid, offset);
        }

        void simRefWrite(Address base, uint32_t tid, int32_t offset, Address value) {
            ManagedMemory::simRefWrite(base, tid, offset, value);
        }

        void simAlloc(Address addr, uint32_t tid, uint32_t size, uint32_t classId) {
            ManagedMemory::simAlloc(addr, tid, size, classId);
        }
};
