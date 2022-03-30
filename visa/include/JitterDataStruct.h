/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#ifndef JITTERDATASTRUCT_
#define JITTERDATASTRUCT_

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
    // The number of named barriers; it also indicates whether kernel uses a
    // barrier.
    unsigned usesBarrier = 0;
    static constexpr unsigned kMaxNamedBarriers = 32;

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
