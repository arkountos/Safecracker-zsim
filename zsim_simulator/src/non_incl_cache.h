#pragma once

#include "cache.h"
#include "mutex.h"
#include "lcp_mem.h"

// A non-inclusive cache that doesn't maintain coherence. Only useful
// for last-level caches.
// - One child and one parent (no dir here)
// - No invalidations (e.g., parent is mem)
// - No network lat
// - No guarantees on locking... the single child should hold its lock

class NonInclusiveCache : public Cache {
    private:
        MemObject* parent;

        const uint32_t tagLat;
        const uint32_t dataLat;
        uint32_t childId;
        MESIState* tag;
        FakeCC* fakeCC;

        //Minimal for now
        Counter profHits, profMisses, profDirtyWBs, profGETNextLevelLat, profGETNetLat;
        Counter profExtraLines;
        mutex cacheLock;
    protected:
        MemObject* getParent(){return parent;}
        uint32_t getChildId(){return childId;}


    public:
        NonInclusiveCache(CC* cc, CacheArray* _array,
                ReplPolicy* _rp, uint32_t _tagLat, uint32_t _dataLat, g_string& _name)
              : Cache(cc, _array, _rp, _tagLat + _dataLat, -1 /*can't use*/, _tagLat,_name)
              , parent(NULL)
              , tagLat(_tagLat)
              , dataLat(_dataLat)
        {
            tag = gm_calloc<MESIState>(_array->getNumLines());
            fakeCC = dynamic_cast<FakeCC*>(cc);
            if (!fakeCC) panic("NonInclusiveCache needs FakeCC --- init.cpp bug?");
            fakeCC->setTags(tag);
        }

        const char* getName() {return name.c_str();}

        FakeCC* getCC() const {return fakeCC;}
        ReplPolicy* getReplPolicy() { return rp; }

        virtual void setParents(uint32_t _childId, const g_vector<MemObject*>& parents, Network* network) {
            //assert(_childId == 0);
            childId = _childId;
            assert(parents.size() == 1);
            parent = parents[0];
        }

        virtual void setChildren(const g_vector<BaseCache*>& children, Network* network) {
            // Nothing to do, children only needed for invalidates which we don't implement
        }

        virtual void initCacheStats(AggregateStat* cacheStat) {
            auto initCounter = [cacheStat] (Counter& c, const char* shortDesc, const char* longDesc) {
                c.init(shortDesc, longDesc);
                cacheStat->append(&c);
            };

            initCounter(profHits, "hits", "GET hits");
            initCounter(profMisses, "misses", "GET misses");
            initCounter(profDirtyWBs, "dirtyWBs", "Dirty writebacks GET misses");
            initCounter(profGETNextLevelLat, "latGETnl", "GET request latency on next level");
            initCounter(profGETNetLat, "latGETnet", "GET request latency on network to next level");
            initCounter(profExtraLines, "extraLines", "Extra lines obtained because of lcp");

            array->initStats(cacheStat);
            rp->initStats(cacheStat);
        }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* cacheStat = new AggregateStat();
            cacheStat->init(name.c_str(), "Cache stats");
            initCacheStats(cacheStat);
            parentStat->append(cacheStat);
        }
        int32_t probe(Address lineAddr) const {
            int32_t lineId = array->lookup(lineAddr, NULL, false, false);
            if (lineId == -1 || !fakeCC->isValid(lineId)) {
                return -1;
            } else {
                return lineId;
            }
        }

        uint64_t access(MemReq& req) {
            // HACK: ignore accesses to 0x0 (they will segfault anyhow, but throw off ZArrays)
            if (unlikely(req.lineAddr == 0)) return req.cycle;

            // We don't issue back-invalidates or have an invalidate path, so
            // unlike for MESI caches simple locking is safe
            scoped_mutex sm(cacheLock);

            bool isGet = (req.type == GETS) || (req.type == GETX);
            int32_t lineId = array->lookup(req.lineAddr, &req, isGet, false);
            doAccessStart(req);
            uint64_t lookupDoneCycle = doTagLookup(req.lineAddr, req.cycle, req.type, req.srcId);
            uint64_t respCycle = lookupDoneCycle;

            if (isGet) {
                //info("NoncluisveCache: GET %lx", req.lineAddr);
                if (lineId == -1 || tag[lineId] == I) { // miss
                //info("      Miss: GET %lx", req.lineAddr);

                    bool doPostinsert = false;
                    if (lineId == -1) { // make space for it
                        Address wbLineAddr;
                        lineId = array->preinsert(req.lineAddr, &req, &wbLineAddr);
                        doPostinsert = (lineId != -1);

                        // assert(lineId != -1);  // with NUCACache, should not bypass

                        if (lineId != -1) {  // make space if not told to bypass
                            if (tag[lineId] != I) {  // dirty writeback
                                uint64_t wbStartCycle = doMissWbDataRead(lineId, lookupDoneCycle);
                                MemReq wbReq = {wbLineAddr, (tag[lineId] == M)? PUTX : PUTS, childId, &tag[lineId],
                                                wbStartCycle, NULL, tag[lineId], req.srcId, 0 /* no flags */};
                                parent->access(wbReq);
                                doMissWbEnd();
                            }
                        }
                    }
                    assert(lineId == -1 || tag[lineId] == I);

                    // NOTE: Parallelized with writeback
                    MemReq parentReq = {req.lineAddr, req.type, childId, &tag[lineId], lookupDoneCycle,
                                        NULL, tag[lineId], req.srcId, 0};
                    if (req.is(MemReq::ZERO_ALLOC)) parentReq.flags |= MemReq::ZERO_ALLOC;
                    respCycle = parent->access(parentReq);
                    //info("NonInclusive Cache parentAccess: lookupdone %ld respCycle %ld", lookupDoneCycle, respCycle);
                    doMissAccEnd();

                    if (lineId != -1) {  // allocate if not told to bypass
                        doMissDataWrite(lineId, respCycle);
                        if (doPostinsert) array->postinsert(req.lineAddr, &req, lineId);
                    } else {
                        assert(!doPostinsert);
                    }

                    profGETNextLevelLat.inc(respCycle - req.cycle);
                    profMisses.inc();
                    auto* lcp = dynamic_cast<LcpVirtualMemory*>(zinfo->vm);
                    if (lcp) {
                        // we are using LCP, so free cache lines coming!
                        uint32_t freeLines = lcp->getNumOfFreeLines(req.lineAddr);
                        //info("          Extra Lines Num: %u", freeLines);
                        profExtraLines.inc(freeLines);
                        for (Address laddr = req.lineAddr + 1; laddr <= req.lineAddr + freeLines; laddr++) {
                            //info("          Extra Lines: GET %lx", laddr);
                            MESIState dummyState;
                            MemReq fakeReq = req;
                            fakeReq = {laddr, GETS, 0, &dummyState, req.cycle, nullptr, I, req.srcId, 0};
                            int32_t elineId = array->lookup(laddr, &fakeReq, true, false);
                            if (elineId == -1 || tag[elineId] == I) { // miss
                                bool doPostinsert = false;
                                if (elineId == -1) { // make space for it
                                    Address wbLineAddr;
                                    elineId = array->preinsert(laddr, &fakeReq, &wbLineAddr);
                                    doPostinsert = (elineId != -1);

                                    // assert(elineId != -1);  // with NUCACache, should not bypass

                                    if (elineId != -1) {  // make space if not told to bypass
                                        if (tag[elineId] != I) {  // dirty writeback
                                            MemReq wbReq = {wbLineAddr, (tag[elineId] == M)? PUTX : PUTS, childId, &tag[elineId],
                                                            fakeReq.cycle, NULL, tag[elineId], req.srcId, 0 /* no flags */};
                                            parent->access(wbReq);
                                        }
                                    }
                                }
                                assert(elineId == -1 || tag[elineId] == I);
                                //poan: skip parent access and just set it valid
                                //MemReq parentReq = {laddr, GETS, childId, &tag[elineId], lookupDoneCycle,
                                //                    NULL, tag[elineId], req.srcId, 0};
                                //parent->access(parentReq);
                                tag[elineId] = S;
                                if (elineId != -1) {  // allocate if not told to bypass
                                    if (doPostinsert) array->postinsert(laddr, &fakeReq, elineId);
                                } else {
                                    assert(!doPostinsert);
                                }
                            }
                        }
                    }
                } else {  // hit
                    //info("      Hit: GET %lx", req.lineAddr);
                    respCycle = doHitDataRead(lineId, lookupDoneCycle);
                    profHits.inc();
                }

                //MESIState state = tag[lineId];
                //assert(state == E || state == M);
                *req.state = (req.type==GETS)? E : M;

                assert(lineId == -1 || tag[lineId] != I);
                // PUT
            } else {
                bool miss = (lineId == -1) || (tag[lineId] == I);

                // If we don't have the line, forward writeback to parent if needed (PUTX == dirty)
                if (miss && req.type == PUTX) {
                    MESIState wbState = M;
                    MemReq wbReq = {req.lineAddr, PUTX, childId, &wbState, lookupDoneCycle, NULL, wbState, req.srcId, MemReq::NONINCLWB};
                    parent->access(wbReq);
                    assert(wbState == I);
                    doPutFwdEnd();
                } else if (req.type == PUTX) {
                    doPutDataWrite(lineId, lookupDoneCycle);
                }

                // fake coherence bookkeeping for repl
                if (!miss) {
                    MESIState* state = &tag[lineId];
                    if (req.type == PUTS) {
                        assert_msg(*req.state == E || *req.state == S, "Req state is %s on PUTS, expected E or S", MESIStateName(*req.state));
                        *state = E;
                    } else {
                        assert_msg(*req.state == E || *req.state == M, "Req state is %s on PUTX, expected E or M", MESIStateName(*req.state));
                        *state = M;
                    }
                }
                if (req.is(MemReq::PUTX_KEEPEXCL)) {
                    assert(*req.state == M);
                    assert(req.type == PUTX);
                    *req.state = E;
                } else {
                    *req.state = I;
                }
            }
            doAccessEnd(req, respCycle);
            return respCycle;
        }

        uint64_t move(const MemReq& req, Cache* dst) {
            Address lineAddr = req.lineAddr;
            // Source, must exist
            int32_t srcLineId = array->lookup(lineAddr, NULL, false, false);
            assert(srcLineId != -1);

            NonInclusiveCache* ndst = dynamic_cast<NonInclusiveCache*>(dst);
            assert(ndst);

            // Destination, may cause eviction (or be invalid!)
            int32_t dstLineId = ndst->array->lookup(lineAddr, &req, true, false);
            if (dstLineId == -1) {
                // Make space for new line
                Address wbLineAddr;
                dstLineId = ndst->array->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
                trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

                if (dstLineId == -1) {
                    tag[srcLineId] = I;
                    return req.cycle + getAccLat();
                }
                if (ndst->tag[dstLineId] != I) {  // dirty writeback
                    uint64_t wbStartCycle = req.cycle;
                    MemReq wbReq = {wbLineAddr, (ndst->tag[dstLineId] == M)? PUTX : PUTS, ndst->childId, &ndst->tag[dstLineId],
                        wbStartCycle, NULL, ndst->tag[dstLineId], req.srcId, 0 /*no flags*/};
                    ndst->parent->access(wbReq);
                    // ndst->tag[dstLineId] = I;
                    // FIXME: not simulating timing here...
                    assert(ndst->tag[dstLineId] == I);
                }

                // No CC calls because we don't invalidate
                ndst->array->postinsert(req.lineAddr, &req, dstLineId); //do the actual insertion. NOTE: Now we must split insert into a 2-phase thing because cc unlocks us.
            }

            assert(dstLineId != -1);

            // Move coherence state
            if (ndst->tag[dstLineId] != I) panic("dst bank %s already has line %lu, no need from src bank %s", ndst->getName(), req.lineAddr, getName());
            assert(ndst->tag[dstLineId] == I);
            assert(tag[srcLineId] != I);
            std::swap(ndst->tag[dstLineId], tag[srcLineId]);

            return req.cycle + getAccLat();
        }

        virtual uint64_t invalidate(const InvReq& req) {
            int32_t lineId = array->lookup(req.lineAddr, nullptr, false, false);
            assert_msg(lineId != -1, "[%s] Invalidate on non-existing address 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
            // TODO: have an invalidate latency; right now assumes single-cycle invalidates
            uint64_t respCycle = req.cycle + 1; // invLat;
            trace(Cache, "[%s] Invalidate start 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
            assert(tag[lineId] != I);
            *req.writeback = tag[lineId] == M;
            tag[lineId] = I;

            return respCycle;
        }

        uint64_t scrubInvalidate(MemReq& req) {
            // this is a noninclusive cache, the line may or may not be in this level.
            int32_t lineId = array->lookup(req.lineAddr, &req, false, false);
            // only use it in the LLC;
            assert(req.is(MemReq::CACHE_SCRUB_1));
            if (lineId != -1) {
                tag[lineId] = I;
            }
            // TODO: have an invalidate latency; right now assumes single-cycle invalidates
            uint64_t respCycle = req.cycle + 1; // invLat;
            return respCycle;
        }

        uint64_t zeroAlloc(MemReq& req) {
            panic("!");
        }

        virtual MESIState getState(Address lineAddr) {
            int32_t lineId = array->lookup(lineAddr, NULL, false, false);
            return (lineId == -1)? I : tag[lineId];
        }
        virtual bool isSharer(Address lineAddr, uint32_t childId) {
            panic("isSharer() unimplemented");
            return false;
        }
        virtual uint64_t prefetch(Address lineAddr, uint64_t reqCycle, uint32_t srcId) {
            panic("prefetch unimplemented in this cache type");
        }

        virtual uint32_t getAccLat() const { return tagLat + dataLat; }
        virtual uint32_t getTagLat() const { return tagLat; }
        virtual uint32_t getDataLat() const { return dataLat; }

    protected:
        /* Tag/data/parent access functions. Return latency, and child classes
         * override them to implement more detailed timing.
         *
         * This is done to allow different implementations of the timing logic,
         * including weave phase models, in subclasses. There is a dependency
         * between the control flow in access() and the methods called here.
         *
         * Subclassed instead of separate class b/c array is needed, but may be
         * better down the road to make it a separate class & generalize to
         * other classes (e.g., TimingCache).
         */
        virtual uint64_t doTagLookup(Address lineAddr, uint64_t cycle, AccessType type, uint32_t srcId) { return cycle + tagLat; }
        virtual uint64_t doHitDataRead(uint64_t lineId, uint64_t cycle) { return cycle + dataLat; }

        virtual uint64_t doMissWbDataRead(uint64_t lineId, uint64_t cycle) { return cycle + dataLat; }
        virtual void doMissWbEnd() {}
        virtual void doMissAccEnd() {}
        virtual uint64_t doMissDataWrite(uint64_t lineId, uint64_t cycle) { return cycle + dataLat; }

        virtual void doPutFwdEnd() {}
        virtual uint64_t doPutDataWrite(uint64_t lineId, uint64_t cycle) { return cycle + dataLat; }

        virtual void doAccessStart(const MemReq& req) {}
        virtual void doAccessEnd(const MemReq& req, uint64_t respCycle) {}
};



