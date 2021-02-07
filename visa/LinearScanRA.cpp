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

#include "DebugInfo.h"
#include "common.h"
#include "SpillManagerGMRF.h"
#include "LocalRA.h"
#include "LinearScanRA.h"
#include "RegAlloc.h"

#include <fstream>
#include <tuple>

using namespace vISA;

extern void getForbiddenGRFs(std::vector<unsigned int>& regNum, G4_Kernel& kernel, unsigned stackCallRegSize, unsigned reserveSpillSize, unsigned reservedRegNum);

LinearScanRA::LinearScanRA(BankConflictPass& b, GlobalRA& g, LivenessAnalysis& liveAnalysis) :
    kernel(g.kernel), builder(g.builder), l(liveAnalysis), mem(g.builder.mem), bc(b), gra(g)
{
    stackCallArgLR = nullptr;
    stackCallRetLR = nullptr;
}

void LinearScanRA::allocForbiddenVector(LSLiveRange* lr)
{
    unsigned size = kernel.getNumRegTotal();
    bool* forbidden = (bool*)mem.alloc(sizeof(bool) * size);
    memset(forbidden, false, size);
    lr->setForbidden(forbidden);
}

void globalLinearScan::allocRetRegsVector(LSLiveRange* lr)
{
    unsigned size = builder.kernel.getNumRegTotal();
    bool* forbidden = (bool*)mem.alloc(sizeof(bool) * size);
    memset(forbidden, false, size);
    lr->setRegGRFs(forbidden);
}

LSLiveRange* LinearScanRA::GetOrCreateLocalLiveRange(G4_Declare* topdcl)
{
    LSLiveRange* lr = gra.getLSLR(topdcl);

    // Check topdcl of operand and setup a new live range if required
    if (!lr)
    {
        lr = new (mem)LSLiveRange() ;
        gra.setLSLR(topdcl, lr);
        allocForbiddenVector(lr);
    }

    MUST_BE_TRUE(lr != NULL, "Local LR could not be created");
    return lr;
}

LSLiveRange* LinearScanRA::CreateLocalLiveRange(G4_Declare* topdcl)
{
    LSLiveRange* lr = new (mem)LSLiveRange();
    gra.setLSLR(topdcl, lr);
    allocForbiddenVector(lr);

    MUST_BE_TRUE(lr != NULL, "Local LR could not be created");
    return lr;
}

class isLifetimeOpCandidateForRemoval
{
public:
    isLifetimeOpCandidateForRemoval(GlobalRA& g) : gra(g)
    {
    }

    GlobalRA& gra;

    bool operator()(G4_INST* inst)
    {
        if (inst->isPseudoKill() ||
            inst->isLifeTimeEnd())
        {
            G4_Declare* topdcl = nullptr;

            if (inst->isPseudoKill())
            {
                topdcl = GetTopDclFromRegRegion(inst->getDst());
            }
            else
            {
                topdcl = GetTopDclFromRegRegion(inst->getSrc(0));
            }

            if (topdcl)
            {
                LSLiveRange* lr = gra.getLSLR(topdcl);
                if (lr->getNumRefs() == 0 &&
                    (topdcl->getRegFile() == G4_GRF ||
                        topdcl->getRegFile() == G4_INPUT))
                {
                    // Remove this lifetime op
                    return true;
                }
            }
        }

        return false;
    }
};

void LinearScanRA::removeUnrequiredLifetimeOps()
{
    // Iterate over all instructions and inspect only
    // pseudo_kills/lifetime.end instructions. Remove
    // instructions that have no other useful instruction.

    for (BB_LIST_ITER bb_it = kernel.fg.begin();
        bb_it != kernel.fg.end();
        bb_it++)
    {
        G4_BB* bb = (*bb_it);
        bb->erase(std::remove_if(bb->begin(), bb->end(), isLifetimeOpCandidateForRemoval(this->gra)),
            bb->end());
    }
}

void LinearScanRA::setLexicalID()
{
    unsigned int id = 1;
    for (auto bb : kernel.fg)
    {
        for (auto curInst : *bb)
        {
            if (curInst->isPseudoKill() ||
                curInst->isLifeTimeEnd())
            {
                curInst->setLexicalId(id);
            }
            else
            {
                curInst->setLexicalId(id++);
            }
        }
    }
}

bool LinearScanRA::hasDstSrcOverlapPotential(G4_DstRegRegion* dst, G4_SrcRegRegion* src)
{
    int dstOpndNumRows = 0;

    if (dst->getBase()->isRegVar())
    {
        G4_Declare* dstDcl = dst->getBase()->asRegVar()->getDeclare();
        if (dstDcl != nullptr)
        {
            int dstOffset = (dstDcl->getOffsetFromBase() + dst->getLeftBound()) / numEltPerGRF<Type_UB>();
            G4_DstRegRegion* dstRgn = dst;
            dstOpndNumRows = dstRgn->getSubRegOff() + dstRgn->getLinearizedEnd() - dstRgn->getLinearizedStart() + 1 > numEltPerGRF<Type_UB>();

            if (src != NULL &&
                src->isSrcRegRegion() &&
                src->asSrcRegRegion()->getBase()->isRegVar())
            {
                G4_SrcRegRegion* srcRgn = src->asSrcRegRegion();
                G4_Declare* srcDcl = src->getBase()->asRegVar()->getDeclare();
                int srcOffset = (srcDcl->getOffsetFromBase() + src->getLeftBound()) / numEltPerGRF<Type_UB>();
                bool srcOpndNumRows = srcRgn->getSubRegOff() + srcRgn->getLinearizedEnd() - srcRgn->getLinearizedStart() + 1 > numEltPerGRF<Type_UB>();

                if (dstOpndNumRows || srcOpndNumRows)
                {
                    if (!(gra.isEvenAligned(dstDcl) && gra.isEvenAligned(srcDcl) &&
                        srcOffset % 2 == dstOffset % 2 &&
                        dstOpndNumRows && srcOpndNumRows))
                    {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

void LinearScanRA::getRowInfo(int size, int& nrows, int& lastRowSize)
{
    if (size <= (int)numEltPerGRF<Type_UW>())
    {
        nrows = 1;
    }
    else
    {
        // nrows is total number of rows, including last row even if it is partial
        nrows = size / numEltPerGRF<Type_UW>();
        // lastrowsize is number of words actually used in last row
        lastRowSize = size % numEltPerGRF<Type_UW>();

        if (size % numEltPerGRF<Type_UW>() != 0)
        {
            nrows++;
        }

        if (lastRowSize == 0)
        {
            lastRowSize = numEltPerGRF<Type_UW>();
        }
    }

    return;
}

unsigned int LinearScanRA::convertSubRegOffFromWords(G4_Declare* dcl, int subregnuminwords)
{
    // Return subreg offset in units of dcl's element size.
    // Input is subregnum in word units.
    unsigned int subregnum;

    subregnum = (subregnuminwords * 2) / dcl->getElemSize();

    return subregnum;
}

void LSLiveRange::recordRef(G4_BB* bb, bool fromEntry)
{
    if (numRefsInFG < 2)
    {
        if (fromEntry)
        {
            numRefsInFG += 2;
        }
        else if (bb != prevBBRef)
        {
            numRefsInFG++;
            prevBBRef = bb;
        }
    }
    if (!fromEntry)
    {
        numRefs++;
    }
}

bool LSLiveRange::isGRFRegAssigned()
{
    MUST_BE_TRUE(topdcl != NULL, "Top dcl not set");
    G4_RegVar* rvar = topdcl->getRegVar();
    bool isPhyRegAssigned = false;

    if (rvar)
    {
        if (rvar->isPhyRegAssigned())
            isPhyRegAssigned = true;
    }

    return isPhyRegAssigned;
}

unsigned int LSLiveRange::getSizeInWords()
{
    int nrows = getTopDcl()->getNumRows();
    int elemsize = getTopDcl()->getElemSize();
    int nelems = getTopDcl()->getNumElems();
    int words = 0;

    if (nrows > 1)
    {
        // If sizeInWords is set, use it otherwise consider entire row reserved
        unsigned int sizeInWords = getTopDcl()->getWordSize();

        if (sizeInWords > 0)
            words = sizeInWords;
        else
            words = nrows * numEltPerGRF<Type_UW>();
    }
    else if (nrows == 1)
    {
        int nbytesperword = 2;
        words = (nelems * elemsize + 1) / nbytesperword;
    }

    return words;
}

// Mark physical register allocated to range lr as not busy
void globalLinearScan::freeAllocedRegs(LSLiveRange* lr, bool setInstID)
{
    int sregnum;

    G4_VarBase* preg = lr->getPhyReg(sregnum);

    MUST_BE_TRUE(preg != NULL,
        "Physical register not assigned to live range. Cannot free regs.");

    unsigned int idx = 0;
    if (setInstID)
    {
        lr->getLastRef(idx);
    }

    if (!lr->isUseUnAvailableReg())
    {
        pregManager.freeRegs(preg->asGreg()->getRegNum(),
            sregnum,
            lr->getSizeInWords(),
            idx);
    }
}

void globalLinearScan::printActives()
{
    std::cout << "====================ACTIVATE START===================" << std::endl;
    for (auto lr : active)
    {
        unsigned int start, end;

        lr->getFirstRef(start);
        lr->getLastRef(end);

        int startregnum, endregnum, startsregnum, endsregnum;
        G4_VarBase* op;
        op = lr->getPhyReg(startsregnum);

        startregnum = endregnum = op->asGreg()->getRegNum();
        endsregnum = startsregnum + (lr->getTopDcl()->getNumElems() * lr->getTopDcl()->getElemSize() / 2) - 1;

        if (lr->getTopDcl()->getNumRows() > 1) {
            endregnum = startregnum + lr->getTopDcl()->getNumRows() - 1;

            if (lr->getTopDcl()->getWordSize() > 0)
            {
                endsregnum = lr->getTopDcl()->getWordSize() % numEltPerGRF<Type_UW>() - 1;
                if (endsregnum < 0) endsregnum = 15;
            }
            else
                endsregnum = 15; // last word in GRF
        }
        if (lr->hasIndirectAccess())
        {
            std::cout << "INDIR: ";
        }
        else
        {
            std::cout << "DIR  : ";
        }
        if (lr->getPreAssigned())
        {
            std::cout << "\tPRE: ";
        }
        else
        {
            std::cout << "\tNOT: ";
        }

        std::cout << lr->getTopDcl()->getName() << "(" << start << ", " << end << ", " << lr->getTopDcl()->getByteSize() << ")";
        std::cout << " (r" << startregnum << "." << startsregnum << ":w - " <<
            "r" << endregnum << "." << endsregnum << ":w)";
        std::cout << std::endl;
    }
    for (int i = 0; i < (int)(numRegLRA); i++)
    {
        std::cout << "\nR" << i << ":";

        if (activeGRF[i].activeInput.size())
        {
            for (auto lr : activeGRF[i].activeLV)
            {
                int startregnum, endregnum, startsregnum, endsregnum;
                G4_VarBase* op;
                op = lr->getPhyReg(startsregnum);

                startregnum = endregnum = op->asGreg()->getRegNum();
                endsregnum = startsregnum + (lr->getTopDcl()->getNumElems() * lr->getTopDcl()->getElemSize() / 2) - 1;

                if (lr->getTopDcl()->getNumRows() > 1) {
                    endregnum = startregnum + lr->getTopDcl()->getNumRows() - 1;

                    if (lr->getTopDcl()->getWordSize() > 0)
                    {
                        endsregnum = lr->getTopDcl()->getWordSize() % numEltPerGRF<Type_UW>() - 1;
                        if (endsregnum < 0) endsregnum = 15;
                    }
                    else
                        endsregnum = 15; // last word in GRF
                }

                std::cout << "\tIN: " << lr->getTopDcl()->getName();
                std::cout << "(r" << startregnum << "." << startsregnum << ":w - " <<
                    "r" << endregnum << "." << endsregnum << ":w)";
                //std::cout << std::endl;
            }
        }

        if (activeGRF[i].activeLV.size())
        {
            // There may be multiple variables take same register with different offsets
            for (auto lr : activeGRF[i].activeLV)
            {
                int startregnum, endregnum, startsregnum, endsregnum;
                G4_VarBase* op;
                op = lr->getPhyReg(startsregnum);

                startregnum = endregnum = op->asGreg()->getRegNum();
                endsregnum = startsregnum + (lr->getTopDcl()->getNumElems() * lr->getTopDcl()->getElemSize() / 2) - 1;

                if (lr->getTopDcl()->getNumRows() > 1) {
                    endregnum = startregnum + lr->getTopDcl()->getNumRows() - 1;

                    if (lr->getTopDcl()->getWordSize() > 0)
                    {
                        endsregnum = lr->getTopDcl()->getWordSize() % numEltPerGRF<Type_UW>() - 1;
                        if (endsregnum < 0) endsregnum = 15;
                    }
                    else
                        endsregnum = 15; // last word in GRF
                }

                std::cout << "\t" << lr->getTopDcl()->getName();
                std::cout << "(r" << startregnum << "." << startsregnum << ":w - " <<
                    "r" << endregnum << "." << endsregnum << ":w)";
            }
        }
    }
    std::cout << "====================ACTIVATE END===================" << std::endl;
}

void globalLinearScan::expireAllActive()
{
    if (active.size() > 0)
    {
        // Expire any remaining ranges
        LSLiveRange* lastActive = active.back();
        unsigned int endIdx;

        lastActive->getLastRef(endIdx);

        expireGlobalRanges(endIdx);
    }
}

void LinearScanRA::linearScanMarkReferencesInOpnd(G4_Operand* opnd, bool isEOT, bool isCall)
{
    G4_Declare* topdcl = NULL;

    if ((opnd->isSrcRegRegion() || opnd->isDstRegRegion()))
    {
        topdcl = GetTopDclFromRegRegion(opnd);

        if (topdcl &&
            (topdcl->getRegFile() == G4_GRF || topdcl->getRegFile() == G4_INPUT))
        {
            // Handle GRF here
            MUST_BE_TRUE(topdcl->getAliasDeclare() == NULL, "Not topdcl");
            LSLiveRange* lr = GetOrCreateLocalLiveRange(topdcl);

            lr->recordRef(curBB_, false);
            if (isEOT)
            {
                lr->markEOT();
            }
            if (topdcl->getRegVar()
                && topdcl->getRegVar()->isPhyRegAssigned()
                && topdcl->getRegVar()->getPhyReg()->isGreg())
            {
                lr->setPreAssigned(true);
            }
            if (isCall)
            {
                lr->setIsCall(true);
            }
            if (topdcl->getRegFile() == G4_INPUT)
            {
                BBVector[curBB_->getId()]->setRefInput(true);
            }
        }
    }
    else if (opnd->isAddrExp())
    {
        G4_AddrExp* addrExp = opnd->asAddrExp();

        topdcl = addrExp->getRegVar()->getDeclare();
        while (topdcl->getAliasDeclare() != NULL)
            topdcl = topdcl->getAliasDeclare();

        MUST_BE_TRUE(topdcl != NULL, "Top dcl was null for addr exp opnd");

        LSLiveRange* lr = GetOrCreateLocalLiveRange(topdcl);

        lr->recordRef(curBB_, false);
        lr->markIndirectRef(true);
        if (topdcl->getRegVar()
            && topdcl->getRegVar()->isPhyRegAssigned()
            && topdcl->getRegVar()->getPhyReg()->isGreg())
        {
            lr->setPreAssigned(true);
        }
        if (topdcl->getRegFile() == G4_INPUT)
        {
            BBVector[curBB_->getId()]->setRefInput(true);
        }
    }
}

void LinearScanRA::linearScanMarkReferencesInInst(INST_LIST_ITER inst_it)
{
    auto inst = (*inst_it);

    // Scan dst
    G4_Operand* dst = inst->getDst();
    if (dst)
    {
        linearScanMarkReferencesInOpnd(dst, false, inst->isCall());
    }

    // Scan srcs
    for (int i = 0, nSrcs = inst->getNumSrc(); i < nSrcs; i++)
    {
        G4_Operand* src = inst->getSrc(i);

        if (src)
        {
            linearScanMarkReferencesInOpnd(src, inst->isEOT(), inst->isCall());
        }
    }
}

void LinearScanRA::linearScanMarkReferences(unsigned int& numRowsEOT)
{
    // Iterate over all BBs
    for (auto curBB : kernel.fg)
    {
        curBB_ = curBB;
        // Iterate over all insts
        for (INST_LIST_ITER inst_it = curBB->begin(), inst_end = curBB->end(); inst_it != inst_end; ++inst_it)
        {
            G4_INST* curInst = (*inst_it);

            if (curInst->isPseudoKill() ||
                curInst->isLifeTimeEnd())
            {
                if (curInst->isLifeTimeEnd())
                {
                    linearScanMarkReferencesInInst(inst_it);
                }
                continue;
            }

            if (curInst->isEOT() && kernel.fg.builder->hasEOTGRFBinding())
            {
                numRowsEOT += curInst->getSrc(0)->getTopDcl()->getNumRows();

                if (curInst->isSplitSend() && !curInst->getSrc(1)->isNullReg())
                {
                    // both src0 and src1 have to be >=r112
                    numRowsEOT += curInst->getSrc(1)->getTopDcl()->getNumRows();
                }
            }

            linearScanMarkReferencesInInst(inst_it);
        }

        if (BBVector[curBB->getId()]->hasBackEdgeIn()
            || curBB->getId() == 0)
        {
            for (unsigned i = 0; i < kernel.Declares.size(); i++)
            {
                G4_Declare* dcl = kernel.Declares[i];
                if (dcl->getAliasDeclare() != NULL)
                {
                    continue;
                }

                if (dcl->getRegFile() != G4_GRF && dcl->getRegFile() != G4_INPUT)
                {
                    continue;
                }

                LSLiveRange* lr = gra.getSafeLSLR(dcl);
                if (lr == nullptr)
                {
                    continue;
                }

                if (l.isLiveAtEntry(curBB, dcl->getRegVar()->getId()))
                {
                    lr->recordRef(curBB, true);
                }
            }
        }
    }

    getGlobalDeclares();
}

bool LSLiveRange::isLiveRangeGlobal() const
{
    if (numRefsInFG > 1 ||
        topdcl->isOutput() == true ||
        (topdcl->getRegVar()
        && topdcl->getRegVar()->isPhyRegAssigned()
        && topdcl->getRegVar()->getPhyReg()->isGreg()))
    {
        return true;
    }

    return false;
}

void LinearScanRA::getGlobalDeclares()
{
    for (G4_Declare* dcl : kernel.Declares)
    {
        const LSLiveRange* lr = gra.getSafeLSLR(dcl);

        if (lr && lr->isLiveRangeGlobal())
        {
            globalDeclares.push_back(dcl);
        }
    }

    return;
}

void LinearScanRA::markBackEdges()
{
    unsigned numBBId = (unsigned)kernel.fg.size();
    BBVector.resize(numBBId);

    for (auto curBB : kernel.fg)
    {
        BBVector[curBB->getId()] = new (mem)G4_BB_LS(curBB);
    }

    for (auto curBB : kernel.fg)
    {
        for (auto succBB : curBB->Succs)
        {
            if (curBB->getId() >= succBB->getId())
            {
                BBVector[succBB->getId()]->setBackEdgeIn(true);
                BBVector[curBB->getId()]->setBackEdgeOut(true);
            }
        }
    }
}

void LinearScanRA::createLiveIntervals()
{
    for (auto dcl : gra.kernel.Declares)
    {
        // Mark those physical registers busy that are declared with Output attribute
        // The live interval will gurantee they are not reused.
        if (dcl->isOutput() &&
            dcl->isInput())
        {
            pregs->markPhyRegs(dcl);
        }

        if (dcl->getAliasDeclare() != NULL)
        {
            continue;
        }
        LSLiveRange*lr = new (mem)LSLiveRange();
        gra.setLSLR(dcl, lr);
        allocForbiddenVector(lr);
    }
}

void LinearScanRA::preRAAnalysis()
{
    int numGRF = kernel.getNumRegTotal();

    // Clear LSLiveRange* computed preRA
    gra.clearStaleLiveRanges();

    createLiveIntervals();

    markBackEdges();
    // Mark references made to decls
    linearScanMarkReferences(numRowsEOT);

    // Check whether pseudo_kill/lifetime.end are only references
    // for their respective variables. Remove them if so. Doing this
    // helps reduce number of variables in symbol table increasing
    // changes of skipping global RA.
    removeUnrequiredLifetimeOps();

    numRegLRA = numGRF;

    unsigned int reservedGRFNum = builder.getOptions()->getuInt32Option(vISA_ReservedGRFNum);
    bool hasStackCall = kernel.fg.getHasStackCalls() || kernel.fg.getIsStackCallFunc();
    if (hasStackCall || reservedGRFNum || builder.getOption(vISA_Debug))
    {
        std::vector<unsigned int> forbiddenRegs;
        unsigned int stackCallRegSize = hasStackCall ? gra.kernel.numReservedABIGRF() : 0;
        getForbiddenGRFs(forbiddenRegs, kernel, stackCallRegSize, 0, reservedGRFNum);
        for (unsigned int i = 0, size = forbiddenRegs.size(); i < size; i++)
        {
            unsigned int regNum = forbiddenRegs[i];
            pregs->setGRFUnavailable(regNum); //un-available will always be there, if it's conflict with input or pre-assigned, it's still un-available.
        }

        if (builder.getOption(vISA_Debug))
        {
            // Since LinearScanRA is not undone when debug info generation is required,
            // for keeping compile time low, we allow fewer physical registers
            // as assignable candidates. Without this, we could run in to a
            // situation where very few physical registers are available for GRA
            // and it is unable to assign registers even with spilling.
#define USABLE_GRFS_WITH_DEBUG_INFO 80
            int maxSendReg = 0;
            for (auto bb : kernel.fg)
            {
                for (auto inst : *bb)
                {
                    if (inst->isSend() || inst->isSplitSend())
                    {
                        maxSendReg = (inst->getMsgDesc()->ResponseLength() > maxSendReg) ?
                            (inst->getMsgDesc()->ResponseLength()) : maxSendReg;
                        maxSendReg = (inst->getMsgDesc()->MessageLength() > maxSendReg) ?
                            (inst->getMsgDesc()->MessageLength()) : maxSendReg;
                        maxSendReg = (inst->getMsgDesc()->extMessageLength() > maxSendReg) ?
                            (inst->getMsgDesc()->extMessageLength()) : maxSendReg;
                    }
                }
            }

            int maxRegsToUse = USABLE_GRFS_WITH_DEBUG_INFO;
            if (maxSendReg > (numGRF - USABLE_GRFS_WITH_DEBUG_INFO))
            {
                maxRegsToUse = (numGRF - maxSendReg) - 10;
            }

            // Also check max size of addressed GRF
            unsigned int maxAddressedRows = 0;
            for (auto dcl : kernel.Declares)
            {
                if (dcl->getAddressed() &&
                    maxAddressedRows < dcl->getNumRows())
                {
                    maxAddressedRows = dcl->getNumRows();
                }
            }

            // Assume indirect operand of maxAddressedRows exists
            // on dst, src0, src1. This is overly conservative but
            // should work for general cases.
            if ((numGRF - maxRegsToUse) / 3 < (int)maxAddressedRows)
            {
                maxRegsToUse = (numGRF - (maxAddressedRows * 3));

                if (maxRegsToUse < 0)
                    maxRegsToUse = 0;
            }

            for (int i = maxRegsToUse; i < numGRF; i++)
            {
                pregs->setGRFUnavailable(i);
            }
        }
    }
    else
    {
        pregs->setSimpleGRFAvailable(true);
        const Options* opt = builder.getOptions();
        if (kernel.getInt32KernelAttr(Attributes::ATTR_Target) != VISA_3D ||
            opt->getOption(vISA_enablePreemption) ||
            (kernel.fg.getHasStackCalls() || kernel.fg.getIsStackCallFunc()) ||
            opt->getOption(vISA_ReserveR0))
        {
            pregs->setR0Forbidden();
        }

        if (opt->getOption(vISA_enablePreemption))
        {
            pregs->setR1Forbidden();
        }
    }
    return;
}

void LinearScanRA::getCalleeSaveRegisters()
{
    unsigned int callerSaveNumGRF = builder.kernel.getCallerSaveLastGRF() + 1;
    unsigned int numCalleeSaveRegs = builder.kernel.getNumCalleeSaveRegs();

    gra.calleeSaveRegs.resize(numCalleeSaveRegs, false);
    gra.calleeSaveRegCount = 0;

    G4_Declare* dcl = builder.kernel.fg.pseudoVCEDcl;
    LSLiveRange* lr = gra.getLSLR(dcl);
    const bool* forbidden = lr->getForbidden();
    unsigned int startCalleeSave = builder.kernel.getCallerSaveLastGRF() + 1;
    unsigned int endCalleeSave = startCalleeSave + builder.kernel.getNumCalleeSaveRegs() - 1;
    for (unsigned i = 0; i < builder.kernel.getNumRegTotal(); i++)
    {
        if (forbidden[i])
        {
            if (i >= startCalleeSave && i < endCalleeSave)
            {
                gra.calleeSaveRegs[i - callerSaveNumGRF] = true;
                gra.calleeSaveRegCount++;
            }
        }
    }
}

void LinearScanRA::getCallerSaveRegisters()
{
    unsigned int callerSaveNumGRF = builder.kernel.getCallerSaveLastGRF() + 1;

    for (BB_LIST_ITER it = builder.kernel.fg.begin(); it != builder.kernel.fg.end(); ++it)
    {
        if ((*it)->isEndWithFCall())
        {
            gra.callerSaveRegsMap[(*it)].resize(callerSaveNumGRF, false);
            gra.retRegsMap[(*it)].resize(callerSaveNumGRF, false);
            unsigned callerSaveRegCount = 0;
            G4_INST* callInst = (*it)->back();
            ASSERT_USER((*it)->Succs.size() == 1, "fcall basic block cannot have more than 1 successor");
            G4_Declare* dcl = builder.kernel.fg.fcallToPseudoDclMap[callInst->asCFInst()].VCA->getRegVar()->getDeclare();
            LSLiveRange* lr = gra.getLSLR(dcl);

            const bool* forbidden = lr->getForbidden();
            unsigned int startCalleeSave = 1;
            unsigned int endCalleeSave = startCalleeSave + builder.kernel.getCallerSaveLastGRF();
            for (unsigned i = 0; i < builder.kernel.getNumRegTotal(); i++)
            {
                if (forbidden[i])
                {
                    if (i >= startCalleeSave && i < endCalleeSave)
                    {
                        gra.callerSaveRegsMap[(*it)][i] = true;
                        callerSaveRegCount++;
                    }
                }
            }

            //ret
            const bool* rRegs = lr->getRetGRFs();
            if (rRegs != nullptr)
            {
                for (unsigned i = 0; i < builder.kernel.getNumRegTotal(); i++)
                {
                    if (rRegs[i])
                    {
                        if (i >= startCalleeSave && i < endCalleeSave)
                        {
                            gra.retRegsMap[(*it)][i] = true;
                        }
                    }
                }
            }

            gra.callerSaveRegCountMap[(*it)] = callerSaveRegCount;
        }
    }
}

void LinearScanRA::getSaveRestoreRegister()
{
    if (!builder.getIsKernel())
    {
        getCalleeSaveRegisters();
    }
    getCallerSaveRegisters();
}

/*
 * Calculate the last lexcial ID of executed instruction if the function is called
 */
void LinearScanRA::calculateFuncLastID()
{
    funcLastLexID.resize(kernel.fg.sortedFuncTable.size());
    for (unsigned i = 0; i < kernel.fg.sortedFuncTable.size(); i++)
    {
        funcLastLexID[i] = 0;
    }

    for (auto func : kernel.fg.sortedFuncTable)
    {
        unsigned fid = func->getId();

        if (fid == UINT_MAX)
        {
            // entry kernel
            continue;
        }

        funcLastLexID[fid] = func->getExitBB()->back()->getLexicalId() * 2;
        for (auto&& callee : func->getCallees())
        {
            if (funcLastLexID[fid] < funcLastLexID[callee->getId()])
            {
                funcLastLexID[fid] = funcLastLexID[callee->getId()];
            }
        }
    }
}

int LinearScanRA::linearScanRA()
{
    std::map<unsigned, std::list<vISA::G4_BB*>> regions;
    std::map<unsigned, std::list<vISA::G4_BB*>>::iterator regionIt;

    G4_BB* entryBB = kernel.fg.getEntryBB();
    for (auto bb : kernel.fg)
    {
        regions[entryBB->getId()].push_back(bb);
    }

    if (kernel.fg.getIsStackCallFunc())
    {
        // Allocate space to store Frame Descriptor
        nextSpillOffset += 32;
        scratchOffset += 32;
    }

    std::list<LSLiveRange*> spillLRs;
    int iterator = 0;
    uint32_t GRFSpillFillCount = 0;
    bool hasStackCall = kernel.fg.getHasStackCalls() || kernel.fg.getIsStackCallFunc();
    int globalScratchOffset = kernel.getInt32KernelAttr(Attributes::ATTR_SpillMemOffset);
    bool useScratchMsgForSpill = !hasStackCall && (globalScratchOffset < (int)(SCRATCH_MSG_LIMIT * 0.6));
    bool enableSpillSpaceCompression = builder.getOption(vISA_SpillSpaceCompression);
    do {
        spillLRs.clear();
        funcCnt = 0;
        std::vector<LSLiveRange*> eotLiveIntervals;
        inputIntervals.clear();
        setLexicalID();
        calculateFuncLastID();

#ifdef DEBUG_VERBOSE_ON
        COUT_ERROR << "=============  ITERATION: " << iterator << "============" << std::endl;
#endif

        //Input
        PhyRegsLocalRA initPregs = (*pregs);
        calculateInputIntervalsGlobal(initPregs, (*regions.begin()).second);
#ifdef DEBUG_VERBOSE_ON
        COUT_ERROR << "===== printInputLiveIntervalsGlobal============" << kernel.getName() << std::endl;
        printInputLiveIntervalsGlobal();
#endif

        globalLiveIntervals.clear();
        preAssignedLiveIntervals.clear();
        eotLiveIntervals.clear();
        unsigned latestLexID = 0;

        for (regionIt = regions.begin(); regionIt != regions.end(); regionIt++)
        {
#ifdef DEBUG_VERBOSE_ON
            COUT_ERROR << "===== REGION: " << (*regionIt).first << "============" << std::endl;
#endif
            regionID = (*regionIt).first;
            for (auto bb : (*regionIt).second)
            {
                calculateLiveIntervalsGlobal(bb, globalLiveIntervals, eotLiveIntervals);
                latestLexID = bb->back()->getLexicalId() * 2;
            }
        }
#ifdef DEBUG_VERBOSE_ON
        COUT_ERROR << "===== globalLiveIntervals============" << std::endl;
        printLiveIntervals(globalLiveIntervals);
#endif

        if (eotLiveIntervals.size())
        {
            assignEOTLiveRanges(builder, eotLiveIntervals);
            for (auto lr : eotLiveIntervals)
            {
                preAssignedLiveIntervals.push_back(lr);
            }
        }
#ifdef DEBUG_VERBOSE_ON
        COUT_ERROR << "===== preAssignedLiveIntervals============" << std::endl;
        printLiveIntervals(preAssignedLiveIntervals);
#endif

        PhyRegsManager pregManager(initPregs, doBCR);
        globalLinearScan ra(gra, &l, globalLiveIntervals, &preAssignedLiveIntervals, inputIntervals, pregManager,
            mem, numRegLRA, numRowsEOT, latestLexID,
            doBCR, highInternalConflict);
        if (!ra.runLinearScan(builder, globalLiveIntervals, spillLRs))
        {
            undoLinearScanRAAssignments();
            return VISA_FAILURE;
        }

        if (spillLRs.size())
        {
            if (iterator == 0 &&
                enableSpillSpaceCompression &&
                kernel.getInt32KernelAttr(Attributes::ATTR_Target) == VISA_3D &&
                !(kernel.fg.getHasStackCalls() || kernel.fg.getIsStackCallFunc()))
            {
                unsigned int spillSize = 0;
                for (auto lr : spillLRs)
                {
                    spillSize += lr->getTopDcl()->getByteSize();
                }
                if ((spillSize * 1.5) < (SCRATCH_MSG_LIMIT - nextSpillOffset))
                {
                    enableSpillSpaceCompression = false;
                }
            }

            SpillManagerGRF spillGRF(gra,
                nextSpillOffset,
                l.getNumSelectedVar(),
                &l,
                &spillLRs,
                enableSpillSpaceCompression,
                useScratchMsgForSpill);

            spillGRF.spillLiveRanges(&kernel);
            nextSpillOffset = spillGRF.getNextOffset();;
            scratchOffset = std::max(scratchOffset, spillGRF.getNextScratchOffset());
#ifdef DEBUG_VERBOSE_ON
            COUT_ERROR << "===== printSpillLiveIntervals============" << std::endl;
            printSpillLiveIntervals(spillLRs);
#endif
            for (auto lr : spillLRs)
            {
                GRFSpillFillCount += lr->getNumRefs();
            }

            // update jit metadata information for spill
            if (auto jitInfo = builder.getJitInfo())
            {
                jitInfo->isSpill = nextSpillOffset > 0;
                jitInfo->hasStackcalls = kernel.fg.getHasStackCalls();

                if (builder.kernel.fg.frameSizeInOWord != 0) {
                    // jitInfo->spillMemUsed is the entire visa stack size. Consider the caller/callee
                    // save size if having caller/callee save
                    // globalScratchOffset in unit of byte, others in Oword
                    //
                    //                               vISA stack
                    //  globalScratchOffset     -> ---------------------
                    //  FIXME: should be 0-based   |  spill            |
                    //                             |                   |
                    //  calleeSaveAreaOffset    -> ---------------------
                    //                             |  callee save      |
                    //  callerSaveAreaOffset    -> ---------------------
                    //                             |  caller save      |
                    //  paramOverflowAreaOffset -> ---------------------
                    jitInfo->spillMemUsed =
                        builder.kernel.fg.frameSizeInOWord * 16;

                    // reserve spillMemUsed #bytes before 8kb boundary
                    kernel.getGTPinData()->setScratchNextFree(8 * 1024 - kernel.getGTPinData()->getNumBytesScratchUse());
                }
                else {
                    jitInfo->spillMemUsed = nextSpillOffset;
                    kernel.getGTPinData()->setScratchNextFree(nextSpillOffset);
                }
                jitInfo->numGRFSpillFill = GRFSpillFillCount;
            }

            undoLinearScanRAAssignments();
        }

        if (builder.getOption(vISA_RATrace))
        {
            std::cout << "\titeration: " << iterator << "\n";
            std::cout << "\t\tnextSpillOffset: " << nextSpillOffset << "\n";
            std::cout << "\t\tGRFSpillFillCount: " << GRFSpillFillCount << "\n";
        }

        auto underSpillThreshold = [this](int numSpill, int asmCount)
        {
            int threshold = std::min(builder.getOptions()->getuInt32Option(vISA_AbortOnSpillThreshold), 200u);
            return (numSpill * 200) < (threshold * asmCount);
        };

        int instNum = 0;
        for (auto bb : kernel.fg)
        {
            instNum += (int)bb->size();
        }
        if (GRFSpillFillCount && builder.getOption(vISA_AbortOnSpill) && !underSpillThreshold(GRFSpillFillCount, instNum))
        {
            // update jit metadata information
            if (auto jitInfo = builder.getJitInfo())
            {
                jitInfo->isSpill = true;
                jitInfo->spillMemUsed = 0;
                jitInfo->numAsmCount = instNum;
                jitInfo->numGRFSpillFill = GRFSpillFillCount;
            }

            // Early exit when -abortonspill is passed, instead of
            // spending time inserting spill code and then aborting.
            return VISA_SPILL;
        }

        iterator++;
    } while (spillLRs.size() && iterator < MAXIMAL_ITERATIONS);

    if (spillLRs.size())
    {
        std::stringstream spilledVars;
        for (auto dcl : kernel.Declares)
        {
            if (dcl->isSpilled() && dcl->getRegFile() == G4_GRF)
            {
                spilledVars << dcl->getName() << "\t";
            }
        }

        MUST_BE_TRUE(false,
            "ERROR: " << kernel.getNumRegTotal() - builder.getOptions()->getuInt32Option(vISA_ReservedGRFNum)
            << " GRF registers are NOT enough to compile kernel " << kernel.getName() << "!"
            << " The maximum register pressure in the kernel is higher"
            << " than the available physical registers in hardware (even"
            << " with spill code)."
            << " Please consider rewriting the kernel."
            << " Compiling with the symbolic register option and inspecting the"
            << " spilled registers may help in determining the region of high pressure.\n"
            << "The spilling virtual registers are as follows: "
            << spilledVars.str());

        spillLRs.clear();
        return VISA_FAILURE;
    }

    if (kernel.fg.getHasStackCalls() || kernel.fg.getIsStackCallFunc())
    {
        getSaveRestoreRegister();
        unsigned localSpillAreaOwordSize = ROUND(scratchOffset, 64) / 16;
        gra.addSaveRestoreCode(localSpillAreaOwordSize);
    }
    return VISA_SUCCESS;
}

int LinearScanRA::doLinearScanRA()
{
    if (builder.getOption(vISA_RATrace))
    {
        std::cout << "--Global linear Scan RA--\n";
    }
    //Initial pregs which will be used in the preRAAnalysis
    PhyRegsLocalRA phyRegs(&builder, kernel.getNumRegTotal());
    pregs = &phyRegs;
    preRAAnalysis();

    int success = linearScanRA();

    if (success == VISA_SUCCESS)
    {
        kernel.setRAType(RA_Type::GLOBAL_LINEAR_SCAN_RA);
    }

    return success;
}

void LinearScanRA::undoLinearScanRAAssignments()
{
    // Undo all assignments made by local RA
    for (auto dcl : kernel.Declares)
    {
        LSLiveRange* lr = gra.getLSLR(dcl);
        if (lr != NULL)
        {
            if (lr->getAssigned() == true)
            {
                // Undo the assignment
                lr->setAssigned(false);
                if (lr->getTopDcl()->getRegFile() != G4_INPUT &&
                    !lr->getPreAssigned())
                {
                    lr->getTopDcl()->getRegVar()->resetPhyReg();
                }
                lr->resetPhyReg();
            }
            lr->setActiveLR(false);
            lr->setFirstRef(NULL, 0);
            lr->setLastRef(NULL, 0);
            lr->clearForbiddenGRF(kernel.getNumRegTotal());
            lr->setRegionID(-1);
        }
    }
}

void LinearScanRA::setPreAssignedLR(LSLiveRange* lr, std::vector<LSLiveRange*> & preAssignedLiveIntervals)
{
    int subreg = 0;
    G4_VarBase* reg = lr->getPhyReg(subreg);
    unsigned regnum = lr->getTopDcl()->getRegVar()->getPhyReg()->asGreg()->getRegNum();
    if (reg == nullptr)
    {
        unsigned int subReg = lr->getTopDcl()->getRegVar()->getPhyRegOff();
        unsigned int subRegInWord = subReg * lr->getTopDcl()->getRegVar()->getDeclare()->getElemSize() / 2;

        lr->setPhyReg(builder.phyregpool.getGreg(regnum), subRegInWord);
    }
    lr->setAssigned(true);

    //Pre assigned registers may overlap the unavailable registers
    lr->setUseUnAvailableReg(isUseUnAvailableRegister(regnum, lr->getTopDcl()->getNumRows()));

    //Insert into preAssgined live intervals
    //If the pre-assigned register is marked as unavailable, not join the live range
    //FIXME: What about partial overlap?
    if (std::find(preAssignedLiveIntervals.begin(), preAssignedLiveIntervals.end(), lr) == preAssignedLiveIntervals.end())
    {
        preAssignedLiveIntervals.push_back(lr);
    }

    return;
}

void LinearScanRA::setDstReferences(G4_BB* bb, INST_LIST_ITER inst_it, G4_Declare *dcl, std::vector<LSLiveRange*>& liveIntervals, std::vector<LSLiveRange*>& eotLiveIntervals)
{
    G4_INST* curInst = (*inst_it);
    LSLiveRange* lr = gra.getLSLR(dcl);

    if (!lr && dcl->getRegFile() == G4_GRF) //The new variables generated by spill/fill, mark reference should handle it
    {
        lr = CreateLocalLiveRange(dcl);
    }

    if (lr == nullptr ||
        (dcl->getRegFile() == G4_INPUT && dcl != kernel.fg.builder->getStackCallArg() && dcl != kernel.fg.builder->getStackCallRet())||
        (lr->isGRFRegAssigned() && (!dcl->getRegVar()->isGreg())))  //ARF
    {
        return;
    }

    if (dcl == kernel.fg.builder->getStackCallArg())
    {
        if (stackCallArgLR == nullptr)
        {
            lr = new (mem)LSLiveRange();
            stackCallArgLR = lr;
            lr->setTopDcl(dcl);
            allocForbiddenVector(lr);
        }
        else
        {
            lr = stackCallArgLR;
        }
    }
    else if (dcl == kernel.fg.builder->getStackCallRet())
    {
        if (stackCallRetLR == nullptr)
        {
            lr = new (mem)LSLiveRange();
            stackCallRetLR = lr;
            lr->setTopDcl(dcl);
            allocForbiddenVector(lr);
        }
        else
        {
            lr = stackCallRetLR;
        }
    }
    // Check whether local LR is a candidate
    if (lr->isGRFRegAssigned() == false)
    {
        if (lr->getRegionID() != regionID)
        {
            liveIntervals.push_back(lr);
            lr->setRegionID(regionID);
        }

        unsigned int startIdx;
        if (lr->getFirstRef(startIdx) == NULL && startIdx == 0)
        {
            lr->setFirstRef(curInst, curInst->getLexicalId() * 2);
        }
        lr->setLastRef(curInst, curInst->getLexicalId() * 2);
    }
    else if (dcl->getRegVar()->getPhyReg()->isGreg()) //Assigned already and is GRF
    { //Such as stack call varaibles
        unsigned int startIdx;
        if (lr->getRegionID() != regionID)
        {
            if (!curInst->isFCall())
            {
                liveIntervals.push_back(lr);
            }
            lr->setRegionID(regionID);

            //Mark live range as assigned
            setPreAssignedLR(lr, preAssignedLiveIntervals);
        }

        if (lr->getFirstRef(startIdx) == NULL && startIdx == 0)
        {
            lr->setFirstRef(curInst, curInst->getLexicalId() * 2);
        }
        lr->setLastRef(curInst, curInst->getLexicalId() * 2);
    }

    if (lr->isEOT() && std::find(eotLiveIntervals.begin(), eotLiveIntervals.end(), lr) == eotLiveIntervals.end())
    {
        eotLiveIntervals.push_back(lr);
    }

    return;
}

void LinearScanRA::setSrcReferences(G4_BB* bb, INST_LIST_ITER inst_it, int srcIdx, G4_Declare* dcl, std::vector<LSLiveRange*>& liveIntervals, std::vector<LSLiveRange*>& eotLiveIntervals)
{
    G4_INST* curInst = (*inst_it);
    LSLiveRange* lr = gra.getLSLR(dcl);

    if (!lr && dcl->getRegFile() == G4_GRF)
    {
        lr = CreateLocalLiveRange(dcl);
    }

    if (lr == nullptr ||
        (dcl->getRegFile() == G4_INPUT && dcl != kernel.fg.builder->getStackCallRet() && dcl != kernel.fg.builder->getStackCallArg()) ||
        (lr->isGRFRegAssigned() && (!dcl->getRegVar()->isGreg())))  //ARF
    {
        return;
    }

    if (lr->getRegionID() != regionID)
    {
        liveIntervals.push_back(lr);
        lr->setRegionID(regionID);
        gra.addUndefinedDcl(dcl);

        unsigned int startIdx;
        if (lr->getFirstRef(startIdx) == NULL && startIdx == 0)
        {  //Since we scan from front to end, not referenced before means not defined.

            if (lr->isGRFRegAssigned() && dcl->getRegVar()->getPhyReg()->isGreg())
            {
                lr->setFirstRef(nullptr, 1);
                setPreAssignedLR(lr, preAssignedLiveIntervals);
            }
            else //Not pre-asssigned, temp
            {
                lr->setFirstRef(curInst, curInst->getLexicalId() * 2);
            }
        }
    }

    lr->setLastRef(curInst, curInst->getLexicalId() * 2);

    if ((builder.WaDisableSendSrcDstOverlap() &&
        ((curInst->isSend() && srcIdx == 0) ||
            (curInst->isSplitSend() && srcIdx == 1)))
        || (builder.avoidDstSrcOverlap() && curInst->getDst() != NULL && hasDstSrcOverlapPotential(curInst->getDst(), curInst->getSrc(srcIdx)->asSrcRegRegion()))
        )
    {
        lr->setLastRef(curInst, curInst->getLexicalId() * 2 + 1);
    }

    if (lr->isEOT() && std::find(eotLiveIntervals.begin(), eotLiveIntervals.end(), lr) == eotLiveIntervals.end())
    {
        eotLiveIntervals.push_back(lr);
    }

    return;
}

void LinearScanRA::generateInputIntervals(G4_Declare *topdcl, G4_INST* inst, std::vector<uint32_t> &inputRegLastRef, PhyRegsLocalRA& initPregs, bool avoidSameInstOverlap)
{
    unsigned int instID = inst->getLexicalId();
    G4_RegVar* var = topdcl->getRegVar();
    unsigned int regNum = var->getPhyReg()->asGreg()->getRegNum();
    unsigned int regOff = var->getPhyRegOff();
    unsigned int idx = regNum * numEltPerGRF<Type_UW>() +
        (regOff * topdcl->getElemSize()) / G4_WSIZE + topdcl->getWordSize() - 1;

    unsigned int numWords = topdcl->getWordSize();
    for (int i = numWords - 1; i >= 0; --i, --idx)
    {
        if ((inputRegLastRef[idx] == UINT_MAX || inputRegLastRef[idx] < instID) &&
            initPregs.isGRFAvailable(idx / numEltPerGRF<Type_UW>()))
        {
            inputRegLastRef[idx] = instID;
            if (avoidSameInstOverlap)
            {
                inputIntervals.push_front(new (mem)LSInputLiveRange(idx, instID * 2 + 1));
            }
            else
            {
                inputIntervals.push_front(new (mem)LSInputLiveRange(idx, instID * 2));
            }

            if (kernel.getOptions()->getOption(vISA_GenerateDebugInfo))
            {
                updateDebugInfo(kernel, topdcl, 0, inst->getCISAOff());
            }
        }
    }

    initPregs.markPhyRegs(topdcl);

    return;
}

// Generate the input intervals for current BB.
// The input live ranges either live through current BB or killed by current BB.
// So, it's enough we check the live out of the BB and the BB it's self
void LinearScanRA::calculateInputIntervalsGlobal(PhyRegsLocalRA &initPregs, std::list<vISA::G4_BB*> &bbList)
{
    int numGRF = kernel.getNumRegTotal();
    std::vector<uint32_t> inputRegLastRef(numGRF * numEltPerGRF<Type_UW>(), UINT_MAX);

    for (BB_LIST_RITER bb_it = bbList.rbegin(), bb_rend = bbList.rend();
        bb_it != bb_rend;
        bb_it++)
    {
        G4_BB* bb = (*bb_it);

        //@ the end of BB
        if (BBVector[bb->getId()]->hasBackEdgeOut())
        {
            for (auto dcl : globalDeclares)
            {
                if (dcl->getAliasDeclare() != NULL ||
                    dcl->isSpilled())
                    continue;

                if (dcl->getRegFile() == G4_INPUT &&
                    dcl->getRegVar()->isGreg() && //Filter out the architecture registers
                    dcl->isOutput() == false &&  //Input and out should be marked as unavailable
                    !builder.isPreDefArg(dcl) &&  //Not stack call associated variables
                    l.isLiveAtExit(bb, dcl->getRegVar()->getId()))
                {
                    MUST_BE_TRUE(dcl->getRegVar()->isPhyRegAssigned(), "Input variable has no pre-assigned physical register");
                    generateInputIntervals(dcl, bb->getInstList().back(), inputRegLastRef, initPregs, false);
                }
            }
        }

        if (!BBVector[bb->getId()]->hasRefInput())
        {
            continue;
        }

        //@BB
        for (INST_LIST_RITER inst_it = bb->rbegin(), inst_rend = bb->rend();
            inst_it != inst_rend;
            inst_it++)
        {
            G4_INST* curInst = (*inst_it);
            G4_Declare* topdcl = NULL;

            // scan dst operand (may be unnecessary but added for safety)
            if (curInst->getDst() != NULL)
            {
                // Scan dst
                G4_DstRegRegion* dst = curInst->getDst();

                topdcl = GetTopDclFromRegRegion(dst);
                if (topdcl &&
                    topdcl->getRegFile() == G4_INPUT &&
                    topdcl->getRegVar()->isGreg() &&
                    topdcl->isOutput() == false &&
                    !builder.isPreDefArg(topdcl))
                {
                    generateInputIntervals(topdcl, curInst, inputRegLastRef, initPregs, false);
                }
            }

            // Scan src operands
            for (int i = 0, nSrcs = curInst->getNumSrc(); i < nSrcs; i++)
            {
                G4_Operand* src = curInst->getSrc(i);

                if (src == nullptr || src->isNullReg())
                {
                    continue;
                }

                if (src->getTopDcl())
                {
                    topdcl = GetTopDclFromRegRegion(src);

                    if (topdcl && topdcl->getRegFile() == G4_INPUT &&
                        (topdcl->getRegVar()->isGreg()) &&
                        topdcl->isOutput() == false &&
                        !builder.isPreDefArg(topdcl))
                    {
                        // Check whether it is input
                        if (builder.avoidDstSrcOverlap() &&
                            curInst->getDst() != NULL &&
                            hasDstSrcOverlapPotential(curInst->getDst(), src->asSrcRegRegion()))
                        {
                            generateInputIntervals(topdcl, curInst, inputRegLastRef, initPregs, true);
                        }
                        else
                        {
                            generateInputIntervals(topdcl, curInst, inputRegLastRef, initPregs, false);
                        }
                    }
                }
                else if (src->isAddrExp())
                {
                    G4_AddrExp* addrExp = src->asAddrExp();

                    topdcl = addrExp->getRegVar()->getDeclare();
                    while (topdcl->getAliasDeclare() != NULL)
                        topdcl = topdcl->getAliasDeclare();

                    MUST_BE_TRUE(topdcl != NULL, "Top dcl was null for addr exp opnd");

                    if (topdcl->getRegFile() == G4_INPUT &&
                        topdcl->getRegVar()->isGreg() &&
                        topdcl->isOutput() == false &&
                        !builder.isPreDefArg(topdcl))
                    {
                        generateInputIntervals(topdcl, curInst, inputRegLastRef, initPregs, false);
                    }
                }
            }
        }
    }

    return;
}

//
//@ the entry of BB
//
void LinearScanRA::calculateLiveInIntervals(G4_BB* bb, std::vector<LSLiveRange*>& liveIntervals)
{
    //FIXME: The complexity is "block_num * declare_num"
    std::vector<LSLiveRange*> preAssignedLiveIntervals;

    for (auto dcl: globalDeclares)
    {
        if (dcl->getAliasDeclare() != NULL ||
            dcl->getRegFile() == G4_INPUT ||
            dcl->isSpilled())
        {
            continue;
        }

        LSLiveRange* lr = gra.getLSLR(dcl);
        if (lr &&
            l.isLiveAtEntry(bb, dcl->getRegVar()->getId()))
        {
            if (lr->getRegionID() != regionID)
            {
                if (lr->isGRFRegAssigned() && dcl->getRegVar()->isGreg())
                {
                    setPreAssignedLR(lr, preAssignedLiveIntervals);
                }
                else
                {
                    liveIntervals.push_back(lr);
                }
                lr->setRegionID(regionID);
            }

            unsigned curIdx = 0;
            if (lr->getFirstRef(curIdx) == NULL && curIdx == 0) //not referenced before, assigned or not assigned?
            {
                lr->setFirstRef((*bb->begin()), (*bb->begin())->getLexicalId() * 2);
            }
        }
    }

    if (preAssignedLiveIntervals.size()&& bb->getId() == 0) //Should happen in the entry BB
    {
        liveIntervals.insert(liveIntervals.begin(), preAssignedLiveIntervals.begin(), preAssignedLiveIntervals.end());
    }

    return;
}

void LinearScanRA::calculateCurrentBBLiveIntervals(G4_BB* bb, std::vector<LSLiveRange*>& liveIntervals, std::vector<LSLiveRange*>& eotLiveIntervals)
{
    for (INST_LIST_ITER inst_it = bb->begin(), bbend = bb->end();
        inst_it != bbend;
        inst_it++)
    {
        G4_INST* curInst = (*inst_it);
        G4_Declare* topdcl = NULL;

        if (curInst->isPseudoKill() ||
            curInst->isLifeTimeEnd() ||
            curInst->isLabel())
        {
            continue;
        }

        if (curInst->isCall() == true)
        {
            const char* name = kernel.fg.builder->getNameString(kernel.fg.builder->mem, 32, "SCALL_%d", funcCnt++);
            G4_Declare* scallDcl = kernel.fg.builder->createDeclareNoLookup(name, G4_GRF, 1, 1, Type_UD);
            LSLiveRange* lr = CreateLocalLiveRange(scallDcl);

            liveIntervals.push_back(lr);
            lr->setRegionID(regionID);
            lr->setFirstRef(curInst, curInst->getLexicalId() * 2);

            FuncInfo* callee = bb->getCalleeInfo();
            unsigned int funcId = callee->getId();
            lr->setLastRef(curInst, funcLastLexID[funcId]);
            lr->setIsCallSite(true);
        }

        if (curInst->isFCall())
        {
            G4_FCALL* fcall = kernel.fg.builder->getFcallInfo(curInst);
            G4_Declare* arg = kernel.fg.builder->getStackCallArg();
            G4_Declare* ret = kernel.fg.builder->getStackCallRet();
            MUST_BE_TRUE(fcall != NULL, "fcall info not found");

            uint16_t retSize = fcall->getRetSize();
            uint16_t argSize = fcall->getArgSize();
            if (ret && retSize > 0 && ret->getRegVar())
            {
                LSLiveRange* stackCallRetLR = new (mem)LSLiveRange();
                stackCallRetLR->setTopDcl(ret);
                allocForbiddenVector(stackCallRetLR);
                stackCallRetLR->setRegionID(regionID);
                stackCallRetLR->setFirstRef(curInst, curInst->getLexicalId() * 2);
                liveIntervals.push_back(stackCallRetLR);
            }
            if (arg && argSize > 0 && arg->getRegVar())
            {
                assert(stackCallArgLR);
                stackCallArgLR->setLastRef(curInst, curInst->getLexicalId() * 2 - 1); //Minus one so that arguments will not be spilled
                stackCallArgLR = nullptr;
            }
        }

        if (curInst->isFReturn())
        {
            uint16_t retSize = kernel.fg.builder->getRetVarSize();
            if (retSize && stackCallRetLR)
            {
                stackCallRetLR->setLastRef(curInst, curInst->getLexicalId() * 2);
                stackCallRetLR = nullptr;
            }
        }

        // Scan srcs
        for (int i = 0, nSrcs = curInst->getNumSrc(); i < nSrcs; i++)
        {
            G4_Operand* src = curInst->getSrc(i);

            if (src == nullptr || src->isNullReg())
            {
                continue;
            }

            if (src && src->isSrcRegRegion())
            {
                if (src->asSrcRegRegion()->isIndirect())
                {
                    auto pointsToSet = l.getPointsToAnalysis().getAllInPointsTo(src->getBase()->asRegVar());
                    for (auto var : *pointsToSet)
                    {
                        G4_Declare* dcl = var->getDeclare();
                        while (dcl->getAliasDeclare())
                        {
                            dcl = dcl->getAliasDeclare();
                        }

                        setSrcReferences(bb, inst_it, i, dcl, liveIntervals, eotLiveIntervals);
                    }
                }
                else
                {
                    // Scan all srcs
                    topdcl = GetTopDclFromRegRegion(src);
                    if (topdcl)
                    {
                        setSrcReferences(bb, inst_it, i, topdcl, liveIntervals, eotLiveIntervals);
                    }
                }
            }
        }

        // Scan dst
        G4_DstRegRegion* dst = curInst->getDst();
        if (dst)
        {
            if (dst->isIndirect())
            {
                auto pointsToSet = l.getPointsToAnalysis().getAllInPointsTo(dst->getBase()->asRegVar());
                for (auto var : *pointsToSet)
                {
                    G4_Declare* dcl = var->getDeclare();
                    while (dcl->getAliasDeclare())
                    {
                        dcl = dcl->getAliasDeclare();
                    }

                    setDstReferences(bb, inst_it, dcl, liveIntervals, eotLiveIntervals);
                }
            }
            else
            {
                topdcl = GetTopDclFromRegRegion(dst);
                if (topdcl)
                {
                    setDstReferences(bb, inst_it, topdcl, liveIntervals, eotLiveIntervals);
                }
            }
        }
    }

    return;
}

void LinearScanRA::calculateLiveOutIntervals(G4_BB* bb, std::vector<LSLiveRange*>& liveIntervals)
{
    for (auto dcl : globalDeclares)
    {
        if (dcl->getAliasDeclare() != NULL ||
            dcl->getRegFile() == G4_INPUT ||
            dcl->isSpilled())
            continue;

        LSLiveRange* lr = gra.getLSLR(dcl);
        if (lr &&
            l.isLiveAtExit(bb, dcl->getRegVar()->getId()))
        {
            lr->setLastRef(bb->getInstList().back(), bb->getInstList().back()->getLexicalId() * 2 + 1);
        }
    }

    return;
}

//
// Live intervals:
// 1. not input variables
// 2. variables without assigned value: normal Intervals.
// 3. variables without assigned value, without define: wired, added by front end. Such as cmp f1.0,  v11, v11. @BB only
// 4. variables which are pre-defined with registers: such as stack call pre-defined varaibles. @BB only
// 5. variables which are pre-defined but will not be assigned with registers: such as %null.  exclusive
// 6. variables which are assigned in previuos region (BB, BBs or function, ..).  //@entry BB
// 7. live in of region: pre-assigned, or not.
// 8. live out of region: set the last reference.
//
void LinearScanRA::calculateLiveIntervalsGlobal(G4_BB* bb, std::vector<LSLiveRange*>& liveIntervals, std::vector<LSLiveRange*>& eotLiveIntervals)
{
    //@ the entry of BB
    if (bb->getId() == 0 ||
        BBVector[bb->getId()]->hasBackEdgeIn())
    {
        calculateLiveInIntervals(bb, liveIntervals);
    }

    //@ BB
    calculateCurrentBBLiveIntervals(bb, liveIntervals, eotLiveIntervals);

    //@ the exit of BB
    if (BBVector[bb->getId()]->hasBackEdgeOut())
    {
        calculateLiveOutIntervals(bb, liveIntervals);
    }

    return;
}

void LinearScanRA::printLiveIntervals(std::vector<LSLiveRange*>& liveIntervals)
{
    for (auto lr : liveIntervals)
    {
        unsigned int start, end;

        lr->getFirstRef(start);
        lr->getLastRef(end);

        std::cout << lr->getTopDcl()->getName() << "(" << start << ", " << end << ", " << lr->getTopDcl()->getByteSize() << ")";
        std::cout << std::endl;
    }

    std::cout << std::endl;
}

void LinearScanRA::printSpillLiveIntervals(std::list<LSLiveRange*>& liveIntervals)
{
    for (auto lr : liveIntervals)
    {
        unsigned int start, end;

        lr->getFirstRef(start);
        lr->getLastRef(end);

        std::cout << lr->getTopDcl()->getName() << "(" << start << ", " << end << ", " << lr->getTopDcl()->getByteSize() << ")";
        std::cout << std::endl;
    }

    std::cout << std::endl;
}

void LinearScanRA::printInputLiveIntervalsGlobal()
{
    COUT_ERROR << std::endl << "Input Live intervals " << std::endl;

    for (std::list<LSInputLiveRange*>::iterator it = inputIntervals.begin();
        it != inputIntervals.end();
        it++)
    {
        unsigned int regWordIdx, lrEndIdx, regNum, subRegInWord;

        LSInputLiveRange* lr = (*it);

        regWordIdx = lr->getRegWordIdx();
        regNum = regWordIdx / numEltPerGRF<Type_UW>();
        subRegInWord = regWordIdx % numEltPerGRF<Type_UW>();
        lrEndIdx = lr->getLrEndIdx();

        COUT_ERROR << "r" << regNum << "." << subRegInWord << " " << lrEndIdx;
        COUT_ERROR << std::endl;
    }

    COUT_ERROR << std::endl;
}

static inline void printLiveInterval(LSLiveRange* lr, bool assign)
{
    int startregnum, endregnum, startsregnum, endsregnum;
    G4_VarBase* op;
    op = lr->getPhyReg(startsregnum);

    startregnum = endregnum = op->asGreg()->getRegNum();
    endsregnum = startsregnum + (lr->getTopDcl()->getNumElems() * lr->getTopDcl()->getElemSize() / 2) - 1;

    if (lr->getTopDcl()->getNumRows() > 1) {
        endregnum = startregnum + lr->getTopDcl()->getNumRows() - 1;

        if (lr->getTopDcl()->getWordSize() > 0)
        {
            endsregnum = lr->getTopDcl()->getWordSize() % numEltPerGRF<Type_UW>() - 1;
            if (endsregnum < 0) endsregnum = 15;
        }
        else
            endsregnum = 15; // last word in GRF
    }
    if (assign)
    {
        COUT_ERROR << "Assigned physical register to ";
    }
    else
    {
        COUT_ERROR << "Free physical register of ";
    }
    COUT_ERROR << lr->getTopDcl()->getName() <<
        " (r" << startregnum << "." << startsregnum << ":w - " <<
        "r" << endregnum << "." << endsregnum << ":w)" << std::endl;

    return;
}

globalLinearScan::globalLinearScan(GlobalRA& g, LivenessAnalysis* l, std::vector<LSLiveRange*>& lv, std::vector<LSLiveRange*>* assignedLiveIntervals,
    std::list<LSInputLiveRange*, std_arena_based_allocator<LSInputLiveRange*>>& inputLivelIntervals,
    PhyRegsManager& pregMgr, Mem_Manager& memmgr,
    unsigned int numReg, unsigned int numEOT, unsigned int lastLexID, bool bankConflict,
    bool internalConflict)
    : gra(g)
    , builder(g.builder)
    , mem(memmgr)
    , pregManager(pregMgr)
    , liveIntervals(lv)
    , preAssignedIntervals(assignedLiveIntervals)
    , inputIntervals(inputLivelIntervals)
    , numRowsEOT(numEOT)
    , lastLexicalID(lastLexID)
    , numRegLRA(numReg)
    , doBankConflict(bankConflict)
    , highInternalConflict(internalConflict)
{
    startGRFReg = 0;
    activeGRF.resize(g.kernel.getNumRegTotal());
    for (auto lr : inputLivelIntervals)
    {
        unsigned int regnum = lr->getRegWordIdx() / numEltPerGRF<Type_UW>();
        activeGRF[regnum].activeInput.push_back(lr);
    }
}

void globalLinearScan::getCalleeSaveGRF(std::vector<unsigned int>& regNum, G4_Kernel* kernel)
{
    unsigned int startCallerSave = kernel->calleeSaveStart();
    unsigned int endCallerSave = startCallerSave + kernel->getNumCalleeSaveRegs();

    for (auto active_it = active.begin();
        active_it != active.end();
        active_it++)
    {
        LSLiveRange* lr = (*active_it);

        G4_VarBase* op;
        int startsregnum = 0;
        op = lr->getPhyReg(startsregnum);
        unsigned startregnum = op->asGreg()->getRegNum();
        unsigned endregnum = startregnum + lr->getTopDcl()->getNumRows() - 1;

        for (unsigned i = startregnum; i <= endregnum; i++)
        {
            if (i >= startCallerSave && i <= endCallerSave)
            {
                regNum.push_back(i);
            }
        }
    }

    return;
}

void globalLinearScan::getCallerSaveGRF(
    std::vector<unsigned int>& regNum,
    std::vector<unsigned int>& retRegNum,
    G4_Kernel* kernel)
{
    unsigned int startCalleeSave = 1;
    unsigned int endCalleeSave = startCalleeSave + kernel->getCallerSaveLastGRF();

    for (auto active_it = active.begin();
        active_it != active.end();
        active_it++)
    {
        LSLiveRange* lr = (*active_it);
        G4_Declare* dcl = lr->getTopDcl();

        if (!builder.kernel.fg.isPseudoVCEDcl(dcl) &&
            !builder.isPreDefArg(dcl))
        {
            G4_VarBase* op;
            int startsregnum = 0;
            op = lr->getPhyReg(startsregnum);
            unsigned startregnum = op->asGreg()->getRegNum();
            unsigned endregnum = startregnum + lr->getTopDcl()->getNumRows() - 1;

            for (unsigned i = startregnum; i <= endregnum; i++)
            {
                if (i >= startCalleeSave && i < endCalleeSave)
                {
                    if (builder.isPreDefRet(dcl))
                    {
                        retRegNum.push_back(i);
                    }
                    else
                    {
                        regNum.push_back(i);
                    }
                }
            }
        }
    }

    for (auto inputlr : inputIntervals)
    {
        unsigned int regnum = inputlr->getRegWordIdx() / numEltPerGRF<Type_UW>();
        std::vector<unsigned int>::iterator it = std::find(regNum.begin(), regNum.end(), regnum);
        if (it == regNum.end())
        {
            if (regnum >= startCalleeSave && regnum < endCalleeSave)
            {
                regNum.push_back(regnum);
            }
        }
    }

}

bool LinearScanRA::isUseUnAvailableRegister(uint32_t startReg, uint32_t regNum)
{
    for (uint32_t i = startReg; i < startReg + regNum; ++i)
    {
        if (!pregs->isGRFAvailable(i))
        {
            return true;
        }
    }

    return false;
}

bool LinearScanRA::assignEOTLiveRanges(IR_Builder& builder, std::vector<LSLiveRange*>& liveIntervals)
{
#ifdef DEBUG_VERBOSE_ON
    COUT_ERROR << "--------------------------------- " << std::endl;
#endif
    uint32_t nextEOTGRF = numRegLRA - numRowsEOT;
    for (auto lr : liveIntervals)
    {
        assert(lr->isEOT());
        G4_Declare* dcl = lr->getTopDcl();
        G4_Greg* phyReg = builder.phyregpool.getGreg(nextEOTGRF);
        dcl->getRegVar()->setPhyReg(phyReg, 0);
        lr->setPhyReg(phyReg, 0);
        lr->setAssigned(true);
        lr->setUseUnAvailableReg(isUseUnAvailableRegister(nextEOTGRF, dcl->getNumRows()));
        nextEOTGRF += dcl->getNumRows();
        if (nextEOTGRF > numRegLRA)
        {
            assert(0);
        }
#ifdef DEBUG_VERBOSE_ON
        printLiveInterval(lr, true);
#endif
    }

    return true;
}

void globalLinearScan::updateCallSiteLiveIntervals(LSLiveRange* callSiteLR)
{
    unsigned lastIdx = 0;
    G4_INST* inst = callSiteLR->getLastRef(lastIdx);

    for (auto lr: active)
    {
        unsigned curLastIdx;
        lr->getLastRef(curLastIdx);
        if (curLastIdx < lastIdx)
        {
            lr->setLastRef(inst, lastIdx);
        }
    }

    for (auto inputlr : inputIntervals)
    {
        unsigned curLastIdx = inputlr->getLrEndIdx();
        if (curLastIdx < lastIdx)
        {
            inputlr->setLrEndIdx(lastIdx);
        }
    }

    return;
}

bool globalLinearScan::runLinearScan(IR_Builder& builder, std::vector<LSLiveRange*>& liveIntervals, std::list<LSLiveRange*>& spillLRs)
{
    unsigned int idx = 0;
    bool allocateRegResult = false;

#ifdef DEBUG_VERBOSE_ON
    COUT_ERROR << "--------------------------------- " <<  std::endl;
#endif

    for (auto lr : liveIntervals)
    {
        G4_Declare* dcl = lr->getTopDcl();
        lr->getFirstRef(idx);
        if (!lr->isEOT() && !lr->getAssigned())
        {
            //Add forbidden for preAssigned registers
            for (auto preAssginedLI : *preAssignedIntervals)
            {
                if (builder.kernel.fg.isPseudoVCADcl(lr->getTopDcl()) &&
                    (builder.isPreDefRet(preAssginedLI->getTopDcl()) ||
                     builder.isPreDefArg(preAssginedLI->getTopDcl())))
                {
                    continue;
                }

                unsigned preFirstIdx, preLastIdx;
                preAssginedLI->getFirstRef(preFirstIdx);
                preAssginedLI->getLastRef(preLastIdx);

                unsigned lastIdx = 0;
                lr->getLastRef(lastIdx);

                if (!(lastIdx < preFirstIdx || preLastIdx < idx))
                {
                    G4_VarBase* preg;
                    int subregnumword;

                    preg = preAssginedLI->getPhyReg(subregnumword);
                    unsigned reg = preg->asGreg()->getRegNum();
                    unsigned rowNum = preAssginedLI->getTopDcl()->getNumRows();

                    for (unsigned k = 0; k < rowNum; k++)
                    {
                        lr->addForbidden(reg + k);
                    }
                }
            }
        }

#ifdef DEBUG_VERBOSE_ON
        COUT_ERROR << "-------- IDX: " << idx << "---------" << std::endl;
#endif

        //Expire the live ranges ended befoe idx
        expireGlobalRanges(idx);
        expireInputRanges(idx);

        if (lr->isCallSite())
        {
            updateCallSiteLiveIntervals(lr);
            continue;
        }

        if (builder.kernel.fg.isPseudoVCADcl(dcl))
        {
            std::vector<unsigned int> callerSaveRegs;
            std::vector<unsigned int> regRegs;
            getCallerSaveGRF(callerSaveRegs, regRegs, &gra.kernel);
            for (unsigned int i = 0; i < callerSaveRegs.size(); i++)
            {
                unsigned int callerSaveReg = callerSaveRegs[i];
                lr->addForbidden(callerSaveReg);
            }
            for (unsigned int i = 0; i < regRegs.size(); i++)
            {
                unsigned int callerSaveReg = regRegs[i];
                if (lr->getRetGRFs() == nullptr)
                {
                    allocRetRegsVector(lr);
                }
                lr->addRetRegs(callerSaveReg);
            }
            continue;
        }
        else if (builder.kernel.fg.isPseudoVCEDcl(dcl))
        {
            calleeSaveLR = lr;
            continue;
        }
        else if (!lr->getAssigned())
        {
            if (dcl == gra.getOldFPDcl())
            {
                std::vector<unsigned int> callerSaveRegs;
                std::vector<unsigned int> regRegs;
                getCallerSaveGRF(callerSaveRegs, regRegs, &gra.kernel);
                for (unsigned int i = 0; i < callerSaveRegs.size(); i++)
                {
                    unsigned int callerSaveReg = callerSaveRegs[i];
                    lr->addForbidden(callerSaveReg);
                }
                for (unsigned int i = 0; i < regRegs.size(); i++)
                {
                    unsigned int callerSaveReg = regRegs[i];
                    if (lr->getRetGRFs() == nullptr)
                    {
                        allocRetRegsVector(lr);
                    }
                    lr->addRetRegs(callerSaveReg);
                }
            }

            //startGRFReg = 0;
            allocateRegResult = allocateRegsLinearScan(lr, builder);
#ifdef DEBUG_VERBOSE_ON
            if (allocateRegResult)
            {
                printLiveInterval(lr, true);
            }
#endif
        }
        else
        {
            allocateRegResult = true;
            int startregnum, subregnum, endsregnum;
            G4_VarBase* op;
            op = lr->getPhyReg(subregnum);

            startregnum = op->asGreg()->getRegNum();
            endsregnum = subregnum + (lr->getTopDcl()->getNumElems() * lr->getTopDcl()->getElemSize() / 2) - 1;
            int nrows = 0;
            int lastRowSize = 0;
            int size = lr->getSizeInWords();
            LinearScanRA::getRowInfo(size, nrows, lastRowSize);

            if (!lr->isUseUnAvailableReg())
            {
                if ((unsigned)size >= numEltPerGRF<Type_UW>())
                {
                    if (size % numEltPerGRF<Type_UW>() == 0)
                    {
                        pregManager.getAvaialableRegs()->setGRFBusy(startregnum, lr->getTopDcl()->getNumRows());
                    }
                    else
                    {
                        pregManager.getAvaialableRegs()->setGRFBusy(startregnum, lr->getTopDcl()->getNumRows() - 1);
                        pregManager.getAvaialableRegs()->setWordBusy(startregnum + lr->getTopDcl()->getNumRows() - 1, 0, lastRowSize);
                    }
                }
                else
                {
                    pregManager.getAvaialableRegs()->setWordBusy(startregnum, subregnum, size);
                }
            }
        }

        if (allocateRegResult)
        {
            updateGlobalActiveList(lr);
        }
        else //Spill
        {
            if (spillFromActiveList(lr, spillLRs))
            {
                //Fixme: get the start GRF already, can allocate immediately
                allocateRegResult = allocateRegsLinearScan(lr, builder);
                if (!allocateRegResult)
                {
#ifdef DEBUG_VERBOSE_ON
                    COUT_ERROR << "Failed assigned physical register to " << lr->getTopDcl()->getName() << ", rows :" << lr->getTopDcl()->getNumRows() << std::endl;
                    printActives();
#endif
                    return false;
                }
                else
                {
                    updateGlobalActiveList(lr);
#ifdef DEBUG_VERBOSE_ON
                    printLiveInterval(lr, true);
#endif
                }
            }
            else
            {
#ifdef DEBUG_VERBOSE_ON
                COUT_ERROR << "Failed to spill registers for " << lr->getTopDcl()->getName() << ", rows :" << lr->getTopDcl()->getNumRows() << std::endl;
                printActives();
#endif
                spillLRs.push_back(lr);
            }
        }
    }

    int totalGRFNum = builder.kernel.getNumRegTotal();
    for (int i = 0; i < totalGRFNum; i++)
    {
        activeGRF[i].activeLV.clear();
        activeGRF[i].activeInput.clear();
    }

    //Assign the registers for the live out ones
    expireAllActive();

    return true;
}

void globalLinearScan::updateGlobalActiveList(LSLiveRange* lr)
{
    bool done = false;
    unsigned int newlr_end;

    lr->getLastRef(newlr_end);

    for (auto active_it = active.begin();
        active_it != active.end();
        active_it++)
    {
        unsigned int end_idx;
        LSLiveRange* active_lr = (*active_it);

        active_lr->getLastRef(end_idx);

        if (end_idx > newlr_end)
        {
            active.insert(active_it, lr);
            done = true;
            break;
        }
    }

    if (done == false)
        active.push_back(lr);

#ifdef DEBUG_VERBOSE_ON
    COUT_ERROR << "Add active " << lr->getTopDcl()->getName() << std::endl;
#endif

    G4_VarBase* op;
    int startsregnum = 0;
    op = lr->getPhyReg(startsregnum);
    unsigned startregnum = op->asGreg()->getRegNum();
    unsigned endregnum = startregnum + lr->getTopDcl()->getNumRows() - 1;
    for (unsigned i = startregnum; i <= endregnum; i++)
    {
        activeGRF[i].activeLV.push_back(lr);
#ifdef DEBUG_VERBOSE_ON
        COUT_ERROR << "Add activeGRF " << lr->getTopDcl()->getName() << " Reg: " << i << std::endl;
#endif
    }
}

bool globalLinearScan::insertLiveRange(std::list<LSLiveRange*>* liveIntervals, LSLiveRange* lr)
{
    unsigned int idx = 0;
    lr->getFirstRef(idx);
    std::list<LSLiveRange*>::iterator it = liveIntervals->begin();
    while (it != liveIntervals->end())
    {
        LSLiveRange* curLR = (*it);
        unsigned curIdx = 0;
        curLR->getFirstRef(curIdx);
        if (curIdx > idx)
        {
            liveIntervals->insert(it, lr);
            return true;
        }

        it++;
    }

    return false;
}

bool globalLinearScan::canBeSpilledLR(LSLiveRange* tlr, LSLiveRange* lr, int GRFNum)
{
    if (lr->isUseUnAvailableReg())
    {
        return false;
    }

    if (lr->isEOT())
    {
        return false;
    }

    if (lr->getTopDcl() == builder.getBuiltinR0())
    {
        return false;
    }

    if (lr->isCall())
    {
        return false;
    }

    if (lr->isGRFRegAssigned())
    {
        return false;
    }

    if (lr->getTopDcl()->isSpilled())
    {
        return false;
    }

    if (lr->getTopDcl()->getRegFile() == G4_INPUT)
    {
        return false;
    }

    if (lr->getTopDcl()->getRegVar()->getId() == UNDEFINED_VAL)
    {
        return false;
    }

    if (lr->getTopDcl()->getRegVar()->isRegVarTransient() || lr->getTopDcl()->getRegVar()->isRegVarTmp())
    {
        return false;
    }

    //Stack call variables
    if (lr->getTopDcl() == gra.getOldFPDcl())
    {
        return false;
    }

    if (builder.kernel.fg.isPseudoVCADcl(lr->getTopDcl()) ||
        builder.kernel.fg.isPseudoVCEDcl(lr->getTopDcl()))
    {
        return false;
    }

    //GRF spill is forbidden for current lr
    const bool* forbidden = lr->getForbidden();
    if (forbidden[GRFNum])
    {
        return false;
    }

    return true;
}

int globalLinearScan::findSpillCandidate(LSLiveRange* tlr)
{
    unsigned short requiredRows = tlr->getTopDcl()->getNumRows();
    int referenceCount = 0;
    int startGRF = -1;
    float spillCost = (float)(int)0x7FFFFFFF;
    unsigned lastIdxs = 1;
    unsigned tStartIdx = 0;

    tlr->getFirstRef(tStartIdx);
    BankAlign bankAlign = getBankAlign(tlr);
    for (int i = 0; i < (int)(numRegLRA - requiredRows); i++)
    {
        unsigned endIdx = 0;
        bool canBeFree = true;
        LSLiveRange* analyzedLV = nullptr;

        pregManager.getAvaialableRegs()->findRegisterCandiateWithAlignForward(i, bankAlign, false);

        // Check the following adjacent registers
        for (int k = i; k < i + requiredRows; k++)
        {
            if (activeGRF[k].activeInput.size() ||
                tlr->getForbidden()[k])
            {
                i = k;
                canBeFree = false;
                break;
            }

            if (activeGRF[k].activeLV.size())
            {
                // There may be multiple variables take same register with different offsets
                for (auto lr : activeGRF[k].activeLV)
                {
                    if (lr == analyzedLV) // one LV may occupy multiple registers
                    {
                        continue;
                    }

                    analyzedLV = lr;

                    if (!canBeSpilledLR(tlr, lr, k))
                    {
                        int startsregnum = 0;
                        G4_VarBase* op = lr->getPhyReg(startsregnum);
                        unsigned startregnum = op->asGreg()->getRegNum();

                        canBeFree = false;
                        i = startregnum + lr->getTopDcl()->getNumRows() - 1; // Jump to k + rows - 1 to avoid unnecessory analysis.

                        break;
                    }

                    int startsregnum = 0;
                    G4_VarBase* op = lr->getPhyReg(startsregnum);
                    int startregnum = op->asGreg()->getRegNum();
                    unsigned effectGRFNum = startregnum > i ? lr->getTopDcl()->getNumRows() : lr->getTopDcl()->getNumRows() - (i - startregnum);
                    lr->getLastRef(endIdx);
                    lastIdxs += (endIdx - tStartIdx) * effectGRFNum;
                    referenceCount += lr->getNumRefs();
                }

                if (!canBeFree)
                {
                    break;
                }
            }
            else if (pregManager.getAvaialableRegs()->isGRFAvailable(k) && !pregManager.getAvaialableRegs()->isGRFBusy(k))
            {
                lastIdxs += lastLexicalID - tStartIdx;
            }
            else //Reserved regsiters
            {
                i = k;
                canBeFree = false;
                break;
            }
        }

        if (canBeFree)
        {
            //Spill cost
            float currentSpillCost = (float)referenceCount / lastIdxs;

            if (currentSpillCost < spillCost)
            {
                startGRF = i;
                spillCost = currentSpillCost;
            }
        }

        lastIdxs = 1;
        referenceCount = 0;
    }

    return startGRF;
}

void globalLinearScan::freeSelectedRegistsers(int startGRF, LSLiveRange* tlr, std::list<LSLiveRange*>& spillLRs)
{
    unsigned short requiredRows = tlr->getTopDcl()->getNumRows();
#ifdef DEBUG_VERBOSE_ON
    COUT_ERROR << "Required GRF size for spill: " << requiredRows << std::endl;
#endif

        //Free registers.
    for (int k = startGRF; k < startGRF + requiredRows; k++)
    {
#ifdef DEBUG_VERBOSE_ON
        if (!activeGRF[k].activeLV.size())
        {
            COUT_ERROR << "Pick free GRF for spill: " << " GRF:" << k << std::endl;
        }
#endif

        while (activeGRF[k].activeLV.size())
        {
            LSLiveRange* lr = activeGRF[k].activeLV.front();

            G4_VarBase* op;
            int startsregnum = 0;
            op = lr->getPhyReg(startsregnum);
            unsigned startregnum = op->asGreg()->getRegNum();
            unsigned endregnum = startregnum + lr->getTopDcl()->getNumRows() - 1;

            assert(startregnum <= (unsigned)k);
            assert(lr->getTopDcl()->getRegFile() != G4_INPUT);

            //Free from the register buckect array
            for (unsigned s = startregnum; s <= endregnum; s++)
            {
                std::vector<LSLiveRange*>::iterator it = std::find(activeGRF[s].activeLV.begin(), activeGRF[s].activeLV.end(), lr);
                if (it != activeGRF[s].activeLV.end())
                {
#ifdef DEBUG_VERBOSE_ON
                    COUT_ERROR << "SPILL: Free activeGRF from : " << (lr)->getTopDcl()->getName() << " GRF:" << s << std::endl;
#endif
                    activeGRF[s].activeLV.erase(it);
                }
            }

#ifdef DEBUG_VERBOSE_ON
            printLiveInterval(lr, false);
#endif

            //Free the allocated register
            freeAllocedRegs(lr, true);

            //Record spilled live range
            if (std::find(spillLRs.begin(), spillLRs.end(), lr) == spillLRs.end())
            {
                spillLRs.push_back(lr);
            }

            //Remove spilled live range from active list
            std::list<LSLiveRange*>::iterator activeListIter = active.begin();
            while (activeListIter != active.end())
            {
                std::list<LSLiveRange*>::iterator nextIt = activeListIter;
                nextIt++;

                if ((*activeListIter) == lr)
                {
#ifdef DEBUG_VERBOSE_ON
                    COUT_ERROR << "SPILL: Free active lr: " << (*activeListIter)->getTopDcl()->getName() << std::endl;
#endif
                    active.erase(activeListIter);
                    break;
                }
                activeListIter = nextIt;
            }
        }
    }
}

bool globalLinearScan::spillFromActiveList(LSLiveRange* tlr, std::list<LSLiveRange*>& spillLRs)
{
    int startGRF = findSpillCandidate(tlr);

    if (startGRF == -1)
    {
#ifdef DEBUG_VERBOSE_ON
        printActives();
#endif
        return false;
    }

    freeSelectedRegistsers(startGRF, tlr, spillLRs);

    return true;
}

void globalLinearScan::expireGlobalRanges(unsigned int idx)
{
    //active list is sorted in ascending order of starting index

    while (active.size() > 0)
    {
        unsigned int endIdx;
        LSLiveRange* lr = active.front();

        lr->getLastRef(endIdx);

        if (endIdx <= idx)
        {
            G4_VarBase* preg;
            int subregnumword, subregnum;

            preg = lr->getPhyReg(subregnumword);

            if (preg)
            {
                subregnum = LinearScanRA::convertSubRegOffFromWords(lr->getTopDcl(), subregnumword);

                // Mark the RegVar object of dcl as assigned to physical register
                lr->getTopDcl()->getRegVar()->setPhyReg(preg, subregnum);
                lr->setAssigned(true);
            }

#ifdef DEBUG_VERBOSE_ON
            printLiveInterval(lr, false);
#endif
            if (preg)
            {
                unsigned startregnum = preg->asGreg()->getRegNum();
                unsigned endregnum = startregnum + lr->getTopDcl()->getNumRows() - 1;
                for (unsigned i = startregnum; i <= endregnum; i++)
                {
                    std::vector<LSLiveRange*>::iterator activeListIter = activeGRF[i].activeLV.begin();
                    while (activeListIter != activeGRF[i].activeLV.end())
                    {
                        std::vector<LSLiveRange*>::iterator nextIt = activeListIter;
                        nextIt++;
                        if ((*activeListIter) == lr)
                        {
                            activeGRF[i].activeLV.erase(activeListIter);
#ifdef DEBUG_VERBOSE_ON
                            COUT_ERROR << "Remove range " << lr->getTopDcl()->getName() << " from activeGRF: " << i << std::endl;
#endif
                            break;
                        }
                        activeListIter = nextIt;
                    }
                }

                if (calleeSaveLR)
                {
                    unsigned int startCallerSave = builder.kernel.calleeSaveStart();
                    unsigned int endCallerSave = startCallerSave + builder.kernel.getNumCalleeSaveRegs();

                    for (unsigned i = startregnum; i <= endregnum; i++)
                    {
                        if (i >= startCallerSave && i <= endCallerSave)
                        {
                            calleeSaveLR->addForbidden(i);
                        }
                    }
                }
            }

            // Free physical regs marked for this range
            freeAllocedRegs(lr, true);

            // Remove range from active list
            active.pop_front();
        }
        else
        {
            // As soon as we find first range that ends after ids break loop
            break;
        }
    }
}

void globalLinearScan::expireInputRanges(unsigned int global_idx)
{
    while (inputIntervals.size() > 0)
    {
        LSInputLiveRange* lr = inputIntervals.front();
        unsigned int endIdx = lr->getLrEndIdx();

        if (endIdx <= global_idx)
        {
            unsigned int regnum = lr->getRegWordIdx() / numEltPerGRF<Type_UW>();
            unsigned int subRegInWord = lr->getRegWordIdx() % numEltPerGRF<Type_UW>();

            // Free physical regs marked for this range
            pregManager.freeRegs(regnum, subRegInWord, 1, endIdx);

#ifdef DEBUG_VERBOSE_ON
            COUT_ERROR << "Expiring input r" << regnum << "." << subRegInWord << std::endl;
#endif

            // Remove range from inputIntervals list
            inputIntervals.pop_front();
            assert(lr == activeGRF[regnum].activeInput.front());
            activeGRF[regnum].activeInput.erase(activeGRF[regnum].activeInput.begin());
        }
        else
        {
            // As soon as we find first range that ends after ids break loop
            break;
        }
    }
}

BankAlign globalLinearScan::getBankAlign(LSLiveRange* lr)
{
    G4_Declare* dcl = lr->getTopDcl();
    BankAlign bankAlign = gra.isEvenAligned(dcl) ? BankAlign::Even : BankAlign::Either;

    if (gra.getVarSplitPass()->isPartialDcl(lr->getTopDcl()))
    {
        // Special alignment is not needed for var split intrinsic
        bankAlign = BankAlign::Either;
    }

    return bankAlign;
}

bool globalLinearScan::allocateRegsLinearScan(LSLiveRange* lr, IR_Builder& builder)
{
    int regnum, subregnum;
    unsigned int localRABound = 0;
    unsigned int instID;

    lr->getFirstRef(instID);
    // Let local RA allocate only those ranges that need < 10 GRFs
    // Larger ranges are not many and are best left to global RA
    // as it can make a better judgement by considering the
    // spill cost.
    int nrows = 0;
    int size = lr->getSizeInWords();
    G4_Declare* dcl = lr->getTopDcl();
    G4_SubReg_Align subalign = gra.getSubRegAlign(dcl);
    localRABound = numRegLRA - 1;

    BankAlign bankAlign = getBankAlign(lr);
    nrows = pregManager.findFreeRegs(size,
        bankAlign,
        subalign,
        regnum,
        subregnum,
        startGRFReg,
        localRABound,
        instID,
        lr->getForbidden());

    if (nrows)
    {
#ifdef DEBUG_VERBOSE_ON
        COUT_ERROR << lr->getTopDcl()->getName() << ":r" << regnum << "  BANK: " << (int)bankAlign << std::endl;
#endif
        lr->setPhyReg(builder.phyregpool.getGreg(regnum), subregnum);
        if (!builder.getOptions()->getOption(vISA_LSFristFit))
        {
            startGRFReg = (startGRFReg + nrows) % localRABound;
        }
        else
        {
            assert(startGRFReg == 0);
        }

        return true;
    }
    else if (!builder.getOptions()->getOption(vISA_LSFristFit))
    {
        startGRFReg = 0;
        nrows = pregManager.findFreeRegs(size,
            bankAlign,
            subalign,
            regnum,
            subregnum,
            startGRFReg,
            localRABound,
            instID,
            lr->getForbidden());
        if (nrows)
        {
#ifdef DEBUG_VERBOSE_ON
            COUT_ERROR << lr->getTopDcl()->getName() << ":r" << regnum << "  BANK: " << (int)bankAlign << std::endl;
#endif
            lr->setPhyReg(builder.phyregpool.getGreg(regnum), subregnum);
            startGRFReg = (startGRFReg + nrows) % localRABound;
            return true;
        }
    }
#ifdef DEBUG_VERBOSE_ON
    COUT_ERROR << lr->getTopDcl()->getName() << ": failed to allocate" << std::endl;
#endif

    return false;
}

bool PhyRegsLocalRA::findFreeMultipleRegsForward(int regIdx, BankAlign align, int& regnum, int nrows, int lastRowSize, int endReg, int instID, const bool* forbidden)
{
    int foundItem = 0;
    int startReg = 0;
    int i = regIdx;
    int grfRows = 0;
    bool multiSteps = nrows > 1;

    if (lastRowSize % numEltPerGRF<Type_UW>() == 0)
    {
        grfRows = nrows;
    }
    else
    {
        grfRows = nrows - 1;
    }

    findRegisterCandiateWithAlignForward(i, align, multiSteps);

    startReg = i;
    while (i <= endReg + nrows - 1)
    {
        if (isGRFAvailable(i) && !forbidden[i] &&
            regBusyVector[i] == 0)
        {
            foundItem++;
        }
        else if (foundItem < grfRows)
        {
            foundItem = 0;
            i++;
            findRegisterCandiateWithAlignForward(i, align, multiSteps);
            startReg = i;
            continue;
        }

        if (foundItem == grfRows)
        {
            if (lastRowSize % numEltPerGRF<Type_UW>() == 0)
            {
                regnum = startReg;
                return true;
            }
            else
            {
                if (i + 1 <= endReg + nrows - 1 &&
                    isGRFAvailable(i + 1) && !forbidden[i + 1] &&
                    (isWordBusy(i + 1, 0, lastRowSize) == false))
                {
                    regnum = startReg;
                    return true;
                }
                else
                {
                    foundItem = 0;
                    i++;
                    findRegisterCandiateWithAlignForward(i, align, multiSteps);
                    startReg = i;
                    continue;
                }
            }
        }

        i++;
    }

    return false;
}

bool PhyRegsLocalRA::findFreeSingleReg(int regIdx, int size, BankAlign align, G4_SubReg_Align subalign, int& regnum, int& subregnum, int endReg, const bool* forbidden)
{
    int i = regIdx;
    bool found = false;

    while (!found)
    {
        if (i > endReg) //<= works
            break;

        // Align GRF
        if ((align == BankAlign::Even) && (i % 2 != 0))
        {
            i++;
            continue;
        }
        else if ((align == BankAlign::Odd) && (i % 2 == 0))
        {
            i++;
            continue;
        }
        else if ((align == BankAlign::Even2GRF) && ((i % 4 >= 2)))
        {
            i++;
            continue;
        }
        else if ((align == BankAlign::Odd2GRF) && ((i % 4 < 2)))
        {
            i++;
            continue;
        }

        if (isGRFAvailable(i, 1) && !forbidden[i])
        {
            found = findFreeSingleReg(i, subalign, regnum, subregnum, size);
            if (found)
            {
                return true;
            }
        }
        i++;
    }

    return false;
}

int PhyRegsManager::findFreeRegs(int size, BankAlign align, G4_SubReg_Align subalign, int& regnum, int& subregnum,
    int startRegNum, int endRegNum, unsigned int instID, const bool* forbidden)
{
    int nrows = 0;
    int lastRowSize = 0;
    LocalRA::getRowInfo(size, nrows, lastRowSize);

    int startReg = startRegNum;
    int endReg = endRegNum - nrows + 1;

    bool found = false;

    if (size >= (int)numEltPerGRF<Type_UW>())
    {
        found = availableRegs.findFreeMultipleRegsForward(startReg, align, regnum, nrows, lastRowSize, endReg, instID, forbidden);
        if (found)
        {
            subregnum = 0;
            if (size % numEltPerGRF<Type_UW>() == 0)
            {
                availableRegs.setGRFBusy(regnum, nrows);
            }
            else
            {
                availableRegs.setGRFBusy(regnum, nrows - 1);
                availableRegs.setWordBusy(regnum + nrows - 1, 0, lastRowSize);
            }
        }
    }
    else
    {
        found = availableRegs.findFreeSingleReg(startReg, size, align, subalign, regnum, subregnum, endReg, forbidden);
        if (found)
        {
            availableRegs.setWordBusy(regnum, subregnum, size);
        }
    }

    if (found)
    {
        return nrows;
    }

    return 0;
}
