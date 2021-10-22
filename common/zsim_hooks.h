#ifndef __ZSIM_HOOKS_H__
#define __ZSIM_HOOKS_H__

#include <stdint.h>
#include <stdio.h>

//Avoid optimizing compilers moving code around this barrier
#define COMPILER_BARRIER() { __asm__ __volatile__("" ::: "memory");}

//These need to be in sync with the simulator
#define ZSIM_MAGIC_OP_ROI_BEGIN         (1025)
#define ZSIM_MAGIC_OP_ROI_END           (1026)
#define ZSIM_MAGIC_OP_REGISTER_THREAD   (1027)
#define ZSIM_MAGIC_OP_HEARTBEAT         (1028)
#define ZSIM_MAGIC_OP_WORK_BEGIN        (1029) //ubik
#define ZSIM_MAGIC_OP_WORK_END          (1030) //ubik

#define ZSIM_MAGIC_OP_SYSTEM_ROI_BEGIN  (4001)
#define ZSIM_MAGIC_OP_RECORD_ALLOCATION (4002)

#ifdef __x86_64__
#define HOOKS_STR  "HOOKS"
static inline void zsim_magic_op(uint64_t op) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op));
    COMPILER_BARRIER();
}
#else
#define HOOKS_STR  "NOP-HOOKS"
static inline void zsim_magic_op(uint64_t op) {
    //NOP
}
#endif

static inline void zsim_roi_begin() {
    printf("[" HOOKS_STR "] ROI begin\n");
    zsim_magic_op(ZSIM_MAGIC_OP_ROI_BEGIN);
}

static inline void zsim_roi_end() {
    zsim_magic_op(ZSIM_MAGIC_OP_ROI_END);
    printf("[" HOOKS_STR  "] ROI end\n");
}

static inline void zsim_heartbeat() {
    zsim_magic_op(ZSIM_MAGIC_OP_HEARTBEAT);
}

static inline void zsim_work_begin() { zsim_magic_op(ZSIM_MAGIC_OP_WORK_BEGIN); }
static inline void zsim_work_end() { zsim_magic_op(ZSIM_MAGIC_OP_WORK_END); }

static inline void zsim_system_roi_begin() {
    zsim_magic_op(ZSIM_MAGIC_OP_SYSTEM_ROI_BEGIN);
    printf("[" HOOKS_STR  "] SYSTEM ROI begin\n");
}

// poan: I need to insert printf or this rdtsc to avoid a weird segfault in the simulator...

#define zsim_record_allocation(addr, size)                       \
    __asm__ __volatile__("xchg %%rcx, %%rcx;"                \
                         :                                  \
                         : "S"(addr), "d"(size), "c"(ZSIM_MAGIC_OP_RECORD_ALLOCATION)    \
                         : "memory");\
    uint32_t hi, lo;\
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));\

#endif /*__ZSIM_HOOKS_H__*/
