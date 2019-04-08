/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

// This file contains implementation of Register Pressure Estimator.

#include "RPE.h"
#include "GraphColor.h"
#include "Timer.h"

namespace vISA
{
    RPE::RPE(const GlobalRA& g, LivenessAnalysis* l) : m(1024), gra(g), live(l->getNumSelectedVar(), false),
        vars(l->vars)
    {
        liveAnalysis = l;
        options = g.kernel.getOptions();
    }

    void RPE::run()
    {
        startTimer(TIMER_RPE);
        if (!vars.empty())
        {
            for (auto& bb : gra.kernel.fg.BBs)
            {
                runBB(bb);
            }
        }
        stopTimer(TIMER_RPE);
    }

    void RPE::runBB(G4_BB* bb)
    {
        G4_Declare* topdcl = nullptr;
        unsigned int id = 0;

        // Compute reg pressure at BB exit
        regPressureBBExit(bb);

        auto updateLivenessForLLR = [this](LocalLiveRange* LLR, bool val)
        {
            int numRows = LLR->getTopDcl()->getNumRows();
            int sreg;
            G4_VarBase* preg = LLR->getPhyReg(sreg);
            int startGRF = preg->asGreg()->getRegNum();
            for (int i = startGRF; i < startGRF + numRows; ++i)
            {
                G4_Declare* GRFDcl = gra.getGRFDclForHRA(i);
                updateLiveness(live, GRFDcl->getRegVar()->getId(), val);
            }
        };

        // Iterate in bottom-up order to analyze register usage (similar to intf graph construction)
        for (auto rInst = bb->rbegin(), rEnd = bb->rend(); rInst != rEnd; rInst++)
        {
            auto inst = (*rInst);
            auto dst = inst->getDst();

            rp[inst] = (uint32_t) regPressure;
            LocalLiveRange* LLR = nullptr;
            if (dst && (topdcl = dst->getTopDcl()))
            {
                if (topdcl->getRegVar()->isRegAllocPartaker())
                {
                    // Check if dst is killed
                    if (liveAnalysis->writeWholeRegion(bb, inst, dst, options) ||
                        inst->isPseudoKill())
                    {
                        id = topdcl->getRegVar()->getId();
                        updateLiveness(live, id, false);
                    }
                }
                else if ((LLR = gra.getLocalLR(topdcl)) && LLR->getAssigned())
                {
                    uint32_t firstRefIdx; 
                    if (LLR->getFirstRef(firstRefIdx) == inst || 
                        liveAnalysis->writeWholeRegion(bb, inst, dst, options))
                    {
                        updateLivenessForLLR(LLR, false);
                    }
                }
            }
            
            for (unsigned int i = 0; i < G4_MAX_SRCS; i++)
            {
                auto src = inst->getSrc(i);
                G4_RegVar* regVar = nullptr;
                
                if (!src)
                    continue;

                if (!src->isSrcRegRegion() || !src->getTopDcl())
                    continue;

                if (!src->asSrcRegRegion()->isIndirect())
                {
                    if ((regVar = src->getTopDcl()->getRegVar()) &&
                        regVar->isRegAllocPartaker())
                    {
                        unsigned int id = regVar->getId();
                        updateLiveness(live, id, true);
                    }
                    else if ((LLR = gra.getLocalLR(src->getTopDcl())) && LLR->getAssigned())
                    {
                        updateLivenessForLLR(LLR, true);
                    }
                }
                else if (src->asSrcRegRegion()->isIndirect())
                {
                    // make every var in points-to set live
                    PointsToAnalysis& pta = liveAnalysis->getPointsToAnalysis();
                    auto pointsToSet = pta.getAllInPointsTo(src->getBase()->asRegVar());
                    if (pointsToSet == nullptr)
                    {
                        // this can happen if the address is coming from addr spill
                        // ToDo: we can avoid this by linking the spilled addr with its new temp addr
                        pointsToSet = pta.getIndrUseVectorPtrForBB(bb->getId());
                    }
                    for (auto var : *pointsToSet)
                    {
                        if (var->isRegAllocPartaker())
                        {
                            updateLiveness(live, var->getId(), true);
                        }
                    }
                }
            }
        }
    }

    void RPE::regPressureBBExit(G4_BB* bb)
    {
        live.clear();
        live = liveAnalysis->use_out[bb->getId()];
        live &= liveAnalysis->def_out[bb->getId()];
    
        // Iterate over all live variables and add up numRows required
        // for each. For scalar variables, add them up separately.
        regPressure = 0;
        unsigned int numScalars = 0;
        for (unsigned int i = 0; i < live.getSize(); i++)
        {
            if (live.isSet(i))
            {
                auto range = vars[i];
                G4_Declare* rootDcl = range->getDeclare()->getRootDeclare();
                if (rootDcl->getNumElems() > 1)
                {
                    regPressure += rootDcl->getNumRows();
                }
                else
                {
                    numScalars++;
                }
            }
        }

        regPressure += numScalars / 8.0;
    }

    void RPE::updateLiveness(BitSet& live, uint32_t id, bool val)
    {
        auto oldVal = live.getElt(id / NUM_BITS_PER_ELT);
        live.set(id, val);
        auto newVal = live.getElt(id / NUM_BITS_PER_ELT);
        updateRegisterPressure(oldVal, newVal, id);
    }

    void RPE::updateRegisterPressure(unsigned int before, unsigned int after, unsigned int id)
    {
        auto change = before^after;
        if (change)
        {
            double delta = vars[id]->getDeclare()->getByteSize() < 32 ? 
                vars[id]->getDeclare()->getByteSize() / 32.0 : (double) vars[id]->getDeclare()->getNumRows();
            if (before & change)
            {
                if (regPressure < delta)
                {
                    regPressure = 0;
                }
                else
                {
                    regPressure -= delta;
                }
            }
            else
            {
                regPressure += delta;
            }
        }
        maxRP = std::max(maxRP, (uint32_t) regPressure);
    }

    void RPE::recomputeMaxRP()
    {
        maxRP = 0;
        // Find max register pressure over all entries in map
        for (auto item : rp)
        {
            maxRP = std::max(maxRP, item.second);
        }
    }

    void RPE::dump() const
    {
        std::cerr << "Max pressure = " << maxRP << "\n";
        for (auto& bb : gra.kernel.fg.BBs)
        {
            for (auto inst : *bb)
            {
                std::cerr << "[";
                if (rp.count(inst))
                {
                    std::cerr << rp.at(inst);
                }
                else
                {
                    std::cerr << "??";
                }
                std::cerr << "]";
                inst->dump();
            }
            std::cerr << "\n";
        }
    }
}
