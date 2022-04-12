/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#ifndef JITTERDATASTRUCT_
#define JITTERDATASTRUCT_

#include <bitset>
#include <optional>
#include <stdint.h>

typedef struct _VISA_BB_INFO{
    int id;
    unsigned staticCycle;
    unsigned sendStallCycle;
    unsigned char loopNestLevel;
} VISA_BB_INFO;

typedef struct _FINALIZER_INFO{
    // Common part
    bool isSpill;
    int numGRFUsed;
    int numAsmCount;

    // spillMemUsed is the scratch size in byte of entire vISA stack for this function/kernel
    // It contains spill size and caller/callee save size.
    unsigned int spillMemUsed = 0;

    // Debug info is callee allocated
    // and populated only if
    // switch is passed to JIT to emit
    // debug info.
    void* genDebugInfo;
    unsigned int genDebugInfoSize;

    // Number of flag spill and fill.
    unsigned numFlagSpillStore;
    unsigned numFlagSpillLoad;

    // Propagate information about barriers presence back to IGC. It's safer to
    // depend on vISA statistics as IGC is not able to detect barriers if they
    // are used as a part of Inline vISA code.
    // This information is used by legacy CMRT as well as OpenCL/L0 runtime.
    //
    // The max number of named barriers allowed
    static constexpr unsigned kMaxNamedBarriers = 32;
    // Use a bitset to track the barrier IDs used
    std::bitset<kMaxNamedBarriers> usedBarriers;
    // Return the max id set + 1 as the number of barriers used. Ideally the
    // number of bits set can be used to represent the number of barriers.
    // However, In current programming model the barriers should be allocated
    // sequentially, so here we return max id + 1 to make sure of that.
    unsigned numBarriers() const {
        auto maxId = getMaxBarrierId();
        return maxId.has_value() ? maxId.value() + 1 : 0;
    }
    // Return true if kernel uses any barrier
    bool hasBarrier() const { return usedBarriers.any(); }
    // Return the max barrier id set
    std::optional<unsigned> getMaxBarrierId() const {
        for (unsigned i = kMaxNamedBarriers - 1; i != static_cast<unsigned>(-1); --i) {
            if (usedBarriers.test(i))
                return std::optional(i);
        }
        return std::nullopt;
    }

    unsigned BBNum;
    VISA_BB_INFO* BBInfo;

    // number of spill/fill, weighted by loop
    unsigned int numGRFSpillFill;
    // whether kernel recompilation should be avoided
    bool avoidRetry = false;

    void* freeGRFInfo;
    unsigned int freeGRFInfoSize;
    unsigned char numBytesScratchGtpin;

    uint32_t offsetToSkipPerThreadDataLoad = 0;
    uint32_t offsetToSkipCrossThreadDataLoad = 0;

    // When two entries prolog is added for setting FFID
    // for compute (GP or GP1), skip this offset to set FFID_GP1.
    // Will set FFID_GP if not skip
    uint32_t offsetToSkipSetFFIDGP = 0;
    uint32_t offsetToSkipSetFFIDGP1 = 0;

    bool hasStackcalls = false;

    uint32_t numGRFTotal = 0;
    uint32_t numThreads = 0;

} FINALIZER_INFO;

#endif // JITTERDATASTRUCT_
