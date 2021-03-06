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

#ifndef PROCESS_TREE_H_
#define PROCESS_TREE_H_

#include "g_std/g_string.h"
#include "g_std/g_vector.h"
#include "g_std/g_unordered_set.h"
#include "galloc.h"
#include "log.h"
#include "mutex.h"
#include "zsim.h"

#ifdef MAXSIM_ENABLED
#include "maxsim/maxsim_interface_c.h"
#endif

class Config;

enum SyncedFastForwardMode {
    SFF_ALWAYS,
    SFF_MULTIPROCESS,
    SFF_NEVER
};

class ProcessTreeNode : public GlobAlloc {
    private:
        g_vector<ProcessTreeNode*> children;
        const char* patchRoot; //used in syscall patching
        uint32_t procIdx;
        const uint32_t groupIdx;
        volatile uint32_t curChildren;
        volatile uint64_t heartbeats;
        bool started;
        volatile bool inFastForward;
        volatile bool inPause;
        uint32_t restartsLeft;
        const SyncedFastForwardMode syncedFastForward;
        const uint32_t clockDomain;
        const uint32_t portDomain;
        const uint64_t dumpHeartbeats;
        const bool dumpsResetHeartbeats;
        const g_vector<bool> mask;
        const g_vector<bool> nullMask;
        const g_vector<uint64_t> ffiPoints;
        const g_string syscallBlacklistRegex;
        mutex nullTidsLock;
        g_unordered_set<uint32_t> nullTids;

    public:
        ProcessTreeNode(uint32_t _procIdx, uint32_t _groupIdx, bool _inFastForward, bool _inPause, const SyncedFastForwardMode& _syncedFastForward,
                        uint32_t _clockDomain, uint32_t _portDomain, uint64_t _dumpHeartbeats, bool _dumpsResetHeartbeats, uint32_t _restarts,
                        const g_vector<bool>& _mask, const g_vector<bool>& _nullMask, const g_vector<uint64_t>& _ffiPoints, const g_string& _syscallBlacklistRegex, const char*_patchRoot)
            : patchRoot(_patchRoot), procIdx(_procIdx), groupIdx(_groupIdx), curChildren(0), heartbeats(0), started(false), inFastForward(_inFastForward),
              inPause(_inPause), restartsLeft(_restarts), syncedFastForward(_syncedFastForward), clockDomain(_clockDomain), portDomain(_portDomain), dumpHeartbeats(_dumpHeartbeats), dumpsResetHeartbeats(_dumpsResetHeartbeats), mask(_mask), nullMask(_nullMask), ffiPoints(_ffiPoints), syscallBlacklistRegex(_syscallBlacklistRegex) {}

        void addChild(ProcessTreeNode* child) {
            children.push_back(child);
        }

        ProcessTreeNode* getNextChild() {
            if (curChildren == children.size()) { //allocate a new child
                uint32_t childProcIdx = __sync_fetch_and_add(&zinfo->numProcs, 1);
                if (childProcIdx >= (uint32_t)zinfo->lineSize) {
                    panic("Cannot simulate more than sys.lineSize=%d processes (to avoid aliasing), limit reached", zinfo->lineSize);
                }
                ProcessTreeNode* child = new ProcessTreeNode(*this);
                child->procIdx = childProcIdx;
                child->started = false;
                child->curChildren = 0;
                child->heartbeats = 0;
                child->children.clear();
                addChild(child);
                zinfo->procArray[childProcIdx] = child;
                info("Created child process %d on the fly, inheriting %d's config", childProcIdx, procIdx);
            }

            assert_msg(curChildren < children.size(), "ProcessTreeNode::getNextChild, procIdx=%d curChildren=%d numChildren=%ld", procIdx, curChildren, children.size());
            return children[curChildren++];
        }

        uint32_t getProcIdx() const {return procIdx;}
        uint32_t getGroupIdx() const {return groupIdx;}

        //Returns true if this is an actual first start, false otherwise (e.g. an exec)
        bool notifyStart();

        //Returns true if this is the last process to end, false otherwise
        bool notifyEnd() __attribute__((warn_unused_result));

        void heartbeat();

        const char* getPatchRoot() const {
            return patchRoot;
        }

        inline bool isInFastForward() const { return inFastForward; }
        inline bool isInPause() const { return inPause; }
        inline bool getSyncedFastForward() const {
            return syncedFastForward == SFF_ALWAYS || (syncedFastForward == SFF_MULTIPROCESS && zinfo->numProcs > 1);
        }

        //In cpp file, they need to access zinfo
        void enterFastForward();
        void exitFastForward();

        inline uint32_t getClockDomain() const {
            return clockDomain;
        }

        inline uint32_t getPortDomain() const {
            return portDomain;
        }

        void exitPause() {
            assert(inPause);
            inPause = false;
            __sync_synchronize();
        }

        const g_vector<bool>& getMask() const {
            return mask;
        }

        const g_vector<bool>& getNullMask() const {
            return nullMask;
        }

        const g_vector<uint64_t>& getFFIPoints() const {
            return ffiPoints;
        }

        const g_string& getSyscallBlacklistRegex() const {
            return syscallBlacklistRegex;
        }

        void registerNullThread(uint32_t tid){ //poan:should add a lock here...
            nullTidsLock.lock();
            info("Register thread tid %d as null thread", tid);
            nullTids.insert(tid);
            for (auto id : nullTids) info("     null thread %d", id);
            nullTidsLock.unlock();
        }

        bool isNullThread(uint32_t tid) {
            info("  isNullthread tid %d %d", tid, nullTids.find(tid) != nullTids.end());
            for (auto id : nullTids) info("     null thread %d", id);

            return  nullTids.find(tid) != nullTids.end();
        }

        //Currently there's no API to get back to a paused state; processes can start in a paused state, but once they are unpaused, they are unpaused for good
};

void CreateProcessTree(Config& config);

void DumpEventualStats(uint32_t procIdx, const char* reason
#ifdef MAXSIM_ENABLED
                       , MaxineVMOperationMode maxineVMOperationMode = MAXINE_VM_OPERATION_MODE_SAME
#endif
);

#endif  // PROCESS_TREE_H_
