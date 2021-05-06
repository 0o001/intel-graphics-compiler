/*========================== begin_copyright_notice ============================

Copyright (C) 2017-2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#pragma once

#include "Compiler/MetaDataUtilsWrapper.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Pass.h>
#include "common/LLVMWarningsPop.hpp"

namespace IGC
{
    /// @brief  This pass sets llvm fast math flags to relevant instructions, according
    ///         to the present compiler options.
    ///         -no-signed-zeros and -unsafe-math-optimizations sets nsz flag
    ///         -finite-math-only sets nnan and ninf flags
    ///         -fast-relaxed-math sets fast flag which implies all others (including arcp)
    class SetFastMathFlags : public llvm::ModulePass
    {
    public:
        /// @brief  Pass identification.
        static char ID;

        SetFastMathFlags();

        ~SetFastMathFlags() {}

        virtual llvm::StringRef getPassName() const override
        {
            return "SetFastMathFlags";
        }

        virtual bool runOnModule(llvm::Module& M) override;

        virtual void getAnalysisUsage(llvm::AnalysisUsage& AU) const override
        {
            AU.setPreservesCFG();
            AU.addRequired<MetaDataUtilsWrapper>();
        }

    private:
        /// @brief  sets the given flags to all instruction supporting fast math flags in the given module.
        /// @param  M - the module
        /// @param  fmfs - the fast math flags
        /// @return true if made any changes to the module.
        static bool setFlags(llvm::Module& M, llvm::FastMathFlags fmfs);
    };

} // namespace IGC
