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
#include "RegDeps.hpp"
#include "Traversals.hpp"
#include "BitSet.hpp"

#include <iterator>

using namespace iga;

/**
 * RAW:                     R kill W    R-->live       explict dependence
 * WAW: different pipelines W2 kill W1  W2-->live      explict dependence
 * WAR: different pipelines W kill R    W-->live       explict dependence
 * WAR: same pipeline       W kill R    W-->live       implict dependence
 * AR: sample pipeline     R2 kill R1  R2-->live      no dependence
 * RAR: different pipelines             R1,R2-->live   no dependence
 *
 * Different pipeline
 * send, math, control flow, long/short (type df)
 *
 * add (8) r10 r20 r30
 * add (8) r11 r21 r22
 * if (..)
 *   // if instruction doesn't count in if calculations, but it takes about 6 cycles to resolve for fall through
 *   // can treat it as continue BB. Only when this BB has one predecessor
 *   add r40 r10 r50 {@2}
 * else
 *   add r60 r70 r80
 * endif
 * //Both control flows converge on this. Conservative analysis start with 1.
 * //By the time jmp happens counter should be at 0 anyway.
 * add r90 r100 r110 {@1}
 * add r91 r101 r111 {@2}
 *
 *
 * Types of Dependencies
 * dst   src0    src1
 * grf   ind     grf //set distance to 1. If SBID list not empty insert test instruction. Optimization if SBID == 1 AND grf depends on it, set SBID, clear SBIDList
 */

/**
    Bucket - represents a GRF line
    ID - sequential numbering of instructions. Resets at 0 with new BB
    Implicit assumption. Various data structures have pointers to Dependencies.

    For each BB scan instructions down
        if new BB
            reset buckets //can use bit mask
            reset distanceTracker
            reset ID
        For each instruction
            Calculate dependcy on srcs and destination
            if currDistance < MAX_DIST
                record distance = currDistance // for a case where we at new BB

            For each dependency and bucket it touches look in to a bucket
                if bucket not empty
                    find potential dependencies //bit mask intersects
                    for each dependency found
                    if appopriate (WAW, RAR, RAW) Dependence exists
                        Clear dependency bits from bucket dependency
                        if dep empty remove from bucket
                        if DistanceDependency //no out of order
                            if instDistance > (currDistance - depID)
                                //We found dependence closer
                                record distance = currDistance - depID //CurrDistance > depID AND min(currDist - depID, 1)
                        else //sbid
                            record SBID ID

            if dependencyRecord NOT empty
                Generate appropriate SWSB/test instruction
                IF SBID
                    if all dependencies are clear
                        add SBID to free list
                        remove entry SBID -> dependencies
            Remove MAX_DIST DEP from buckets
            Add current instruction Dependencies to buckets
            if instruction isVariableExecTime //send, math.idiv
                if freeSBITLIst IS empty
                    pick one SBID
                    generate test instruction
                    move SBID to free list
                    clear dependency from bucket/sbidList

                assign SBID from free list
        if end of block AND SBID list NOT empty
            generate test instructions
*/

#include "SWSBSetter.hpp"
/*
WAW
explicit dependence
math.fc (except idiv) r10 ...
add r10 ....

add r10 ... //long: type DF/Q
add r10 ... //short:

WAW
no dependence
add r10 ...
add r10 ...


Math.sin   r10 r20 r30
Math.cos  r20 r40 r50
Not required - same pipe

Math.sin   r20 r10 r30
Math.cos  r20 r40 r50
Not required - same pipe

FPU_long   r20 r10 r30
Math.sin    r20 r40 r50
Explicit dep required as math can overtake FPU_long - since they are in different pipes.

RAW
add r10 ...
add r20 ...
add ... r20 ... {@1}
add ... r10  {@3} <--- technically speaking this depending is not necesary
                       since they are in same pipe and previous instruction will stall
                       so last instruction dependence is cleared.
                       But in terms of runtime there is no impact so not worth special handling

assuming two grfs are written/read
send r10
send r11

add (16) ... r10 ...
second send has dependency on first send
add has dependency on second send
if sends written 1 grf, and add still read two grfs it will have dependence on both sends

send r10 //set$1 writes r10/r11
add(8) r10 {$1.dst}
add(8) r11 {}


*/
void SWSBAnalyzer::clearDepBuckets(DepSet &depMatch)
{
    for (auto bucketID : depMatch.getBuckets())
    {
        auto bucket = &m_buckets[bucketID];
        auto numDepSets = bucket->getNumDependencies();
        for (uint32_t i = 0; i < numDepSets; ++i)
        {
            DepSet* dep = bucket->getDepSet(i);
            //See if anything matches for this GRF bucket.
            //originally was checking for intersect but was removing extra dependence in case like this
            /*
                (W)      and (1|M0)               r0.0<1>:ud    r0.0<0;1,0>:ud    0xFFFFBFFF:ud    {}
                (W)      mov (16|M0)              r25.0<1>:f    0:d
                (W)      mov (1|M0)               r0.2<1>:ud    0x0:ud
                (W)      mov (1|M0)               r0.2<1>:ud    0x0:ud
                (W)      and (1|M0)               r0.0<1>:ud    r0.0<0;1,0>:ud    0xFFFFF7FF:ud    {}
                         mov (16|M0)              r120.0<1>:ud  r17.0<8;8,1>:ud
                         mov (16|M0)              r122.0<1>:ud  r19.0<8;8,1>:ud
                         mov (16|M0)              r124.0<1>:ud  r21.0<8;8,1>:ud
                         mov (16|M0)              r126.0<1>:ud  r23.0<8;8,1>:ud
                (W)      mov (16|M0)              r118.0<1>:ud  r0.0<8;8,1>:ud                   {}
            */
            //the r0 dependece was already cleared by second r0
            //but when clearing from buckets it would find the second r0 and clear it by mistake
            if (dep && depMatch.getInstGlobalID() == dep->getInstGlobalID() &&
                   (dep->getDepType() == depMatch.getDepType()))
            {
                bucket->clearDepSet(i);
            }
        }
    }
    depMatch.reset();
}
/**
* This function takes in a current instruction dependency.
* Either SRC or DST
* It then checks against previous dependencies.
* It sets mininum valid distance
* and creates an active list of SBIDs this instruction depends on
* It clears and removes previous dependencies.
* The approach is bucket based.
* Each bucket is one GRF.
* So if instruction writes in to more then one GRF then multiple buckets will have the dependency
*/
void SWSBAnalyzer::calculateDependence(DepSet &currDep, SWSB &distanceDependency,
    const Instruction &currInst, vector<SBID>& activeSBID, bool &needSyncForShootDownInst)
{
    needSyncForShootDownInst = false;
    auto currDepType = currDep.getDepType();
    auto currDepPipe = currDep.getDepPipe();

    for (auto bucketID : currDep.getBuckets())
    {
        //iterates over Dependencies in a GRF bucket
        //Assumption there shouldn't be more then 1-2
        Bucket* bucket = &m_buckets[bucketID];
        size_t numDepSets = bucket->getNumDependencies();
        for (uint32_t i = 0; i < numDepSets; ++i)
        {
            uint32_t index = static_cast<uint32_t>(numDepSets -1 - i);
            auto dep = bucket->getDepSet(index);

            if (dep && (dep->getDepType() == DEP_TYPE::WRITE_ALWAYS_INTERFERE ||
                        dep->getDepType() == DEP_TYPE::READ_ALWAYS_INTERFERE))
            {
                // force to sync with dep
                if (dep->getDepClass() == DEP_CLASS::OUT_OF_ORDER)
                {
                    setSbidDependency(*dep, currInst, needSyncForShootDownInst, activeSBID);
                }
                else
                {
                    // Set to sync with all in-order-pipes. WRITE/READ_ALWAYS_INTERFERE
                    // could be used to mark arf dependency, which is required to be all pipes
                    // instead of dep's pipe only
                    distanceDependency.minDist = 1;
                    if (getNumOfDistPipe() == 1)
                        distanceDependency.distType = SWSB::DistType::REG_DIST;
                    bucket->clearDepSet(index);
                }
            }

            //See if anything matches for this GRF bucket.
            if (dep && dep->getBitSet().intersects(currDep.getBitSet()))
            {
                /*
                 * RAW:                     R kill W    R-->live       explict dependence
                 * WAW: different pipelines W2 kill W1  W2-->live      explict dependence
                 * WAR: different pipelines W kill R    W-->live       explict dependence
                 * WAR: same pipeline       W kill R    W-->live       implict dependence
                 * AR: sample pipeline     R2 kill R1  R2-->live      no dependence
                 * RAR: different pipelines             R1,R2-->live   no dependence
                 */
                //RAW:                     R kill W    R-->live       explict dependence
                DEP_TYPE prevDepType = dep->getDepType();
                DEP_PIPE prevDepPipe = dep->getDepPipe();
                DEP_CLASS prevDepClass = dep->getDepClass();

                // Send with different SFID could write to different pipes
                bool send_in_diff_pipe = false;
                if (dep->getInstruction()->getOpSpec().isSendFamily() &&
                    currDep.getInstruction()->getOpSpec().isSendFamily())
                {
                    send_in_diff_pipe =
                        (dep->getInstruction()->getOpSpec().op !=
                         currDep.getInstruction()->getOpSpec().op);
                }

                bool isRAW = currDepType == DEP_TYPE::READ &&
                             prevDepType == DEP_TYPE::WRITE;
                //WAW: different pipelines W2 kill W1  W2-->live      explict dependence
                bool isWAW = (currDepType == DEP_TYPE::WRITE &&
                              prevDepType == DEP_TYPE::WRITE &&
                     (currDepPipe != prevDepPipe || send_in_diff_pipe));
                //WAR: different pipelines W kill R    W-->live       explict dependence
                bool isWAR = currDepType == DEP_TYPE::WRITE &&
                             prevDepType == DEP_TYPE::READ  &&
                             (currDepPipe != prevDepPipe || send_in_diff_pipe);
                bool isWAW_out_of_order
                           = (currDepType == DEP_TYPE::WRITE &&
                              prevDepType == DEP_TYPE::WRITE &&
                              prevDepClass == DEP_CLASS::OUT_OF_ORDER);

                // Special case handling for acc/flag dependency:
                // if the RAW dependency on acc and it's whithin the same pipe,
                // HW can handle it that we don't need to set swsb
                if (isRAW && currDepPipe == prevDepPipe) {
                    auto check_dep_reg = [&](DepSet* in_dep, uint32_t reg_start, uint32_t reg_len) {
                        return in_dep->getBitSet().intersects(currDep.getBitSet(),
                            reg_start, reg_len);
                    };
                    auto has_grf_dep = [&](DepSet* in_dep) {
                        return check_dep_reg(in_dep, m_DB->getGRF_START(), m_DB->getGRF_LEN());
                    };
                    auto has_arf_a_dep = [&](DepSet* in_dep) {
                        return check_dep_reg(in_dep, m_DB->getARF_A_START(), m_DB->getARF_A_LEN());
                    };
                    auto has_acc_dep = [&](DepSet* in_dep) {
                        return check_dep_reg(in_dep, m_DB->getARF_ACC_START(), m_DB->getARF_ACC_LEN());
                    };
                    auto has_flag_dep = [&](DepSet* in_dep) {
                        return check_dep_reg(in_dep, m_DB->getARF_F_START(), m_DB->getARF_F_LEN());
                    };
                    auto has_sp_dep = [&](DepSet* in_dep) {
                        return check_dep_reg(in_dep, m_DB->getARF_SPECIAL_START(), m_DB->getARF_SPECIAL_LEN());
                    };

                    // is acc dependecy
                    if (has_acc_dep(dep)) {
                        // and no dependency on other registers
                        if (!(has_grf_dep(dep) || has_arf_a_dep(dep) || has_flag_dep(dep) || has_sp_dep(dep)))
                            isRAW = false;
                    }
                    // is flag dependency
                    if (has_flag_dep(dep)) {
                        // and no dependency on other registers
                        if (!(has_grf_dep(dep) || has_arf_a_dep(dep) || has_acc_dep(dep) || has_sp_dep(dep)))
                            isRAW = false;
                        // flag and acc only
                        if (has_acc_dep(dep))
                            if (!(has_grf_dep(dep) || has_arf_a_dep(dep) || has_sp_dep(dep)))
                                isRAW = false;
                    }
                }

                if (isWAR ||
                    isWAW ||
                    isRAW ||
                    isWAW_out_of_order)
                {
                    // clearing previous dependence
                    if (dep->getBitSet().empty())
                    {
                        m_errorHandler.reportWarning(
                            currInst.getPC(),
                            "Dependency in bucket with no bits set");
                    }
                    // removing from bucket if there is nothing
                    if (!dep->getBitSet().testAny(bucketID * 32, m_DB->getGRF_BYTES_PER_REG()))
                    {
                        bucket->clearDepSet(index);
                    }
                    if (prevDepClass == DEP_CLASS::IN_ORDER)
                    {
                        if (getNumOfDistPipe() == 1) {
                            // FOR WAW if PREV is SHORT and curr is LONG then write will finish
                            // before current write, no need to set swsb
                            bool isWAWHazard = (prevDepPipe == DEP_PIPE::SHORT && currDepPipe == DEP_PIPE::LONG ||
                                                prevDepPipe == DEP_PIPE::SHORT && currDepPipe == DEP_PIPE::SHORT)
                                               && isWAW;
                            // require swsb for all the other kinds of dependency
                            if (!isWAWHazard)
                            {
                                // setting minimum distance
                                uint32_t newDistance = m_InstIdCounter.inOrder - dep->getInstIDs().inOrder;
                                distanceDependency.minDist =
                                    distanceDependency.minDist == 0 ?
                                    newDistance :
                                    min(distanceDependency.minDist, newDistance);
                                // clamp the distance to max distance
                                distanceDependency.minDist = min(distanceDependency.minDist, (uint32_t)MAX_VALID_DISTANCE);
                                distanceDependency.distType = SWSB::DistType::REG_DIST;
                            }
                        }
                        // clear this instruction's dependency since it is satisfied
                        clearDepBuckets(*dep);

                        // clear its companion because when an in-order instruction is synced, both its
                        // input and output dependency are satisfied. The only case is that if it has
                        // read/write_always_interfere dependency, it should be reserved.
                        // The restriction is that:
                        // When certain Arch Registers (sr, cr, ce) are used,
                        // the very next instruction requires dependency to be set on all pipes {A@1}
                        // e.g.
                        //      mov (1|M0)               r104.0<1>:ud  sr0.1<0;1,0>:ud
                        //      cmp(16 | M0)   (ne)f0.0   null:ud    r104.0<0; 1, 0> : ub   r62.4<0; 1, 0> : uw
                        // A@1 is required for cmp instead of I@1
                        if (dep->getCompanion() != nullptr) {
                            // In the case that this DepSet is generated from math_wa_info, it won't have companion
                            if (dep->getCompanion()->getDepType() != DEP_TYPE::WRITE_ALWAYS_INTERFERE &&
                                dep->getCompanion()->getDepType() != DEP_TYPE::READ_ALWAYS_INTERFERE) {
                                clearDepBuckets(*dep->getCompanion());
                            }
                        }
                    } // end of if (prevDepClass == DEP_CLASS::IN_ORDER)
                    else if (prevDepClass == DEP_CLASS::OUT_OF_ORDER) // prev is out of order
                    {
                        setSbidDependency(*dep, currInst, needSyncForShootDownInst, activeSBID);
                    }
                    // for the instruction in "OTHER" DEP_CLASS, such as sync, we don't need
                    // to consider their dependency that is implied by hardware
                }
            }
        }
    }
}

void SWSBAnalyzer::setSbidDependency(DepSet& dep, const Instruction& currInst,
    bool& needSyncForShootDownInst, vector<SBID>& activeSBID)
{
    /* For out of order we don't know how long it will finish
    * so need to test for SBID.
    * Instruction can depend on more then one SBID
    * send r10
    * send r20
    * send r30
    * ....
    * add r10 r20 r30
    * between different buckets and srcs/dst dependencies instruction can rely on multiple SBID
    */
    SBID depSBID = dep.getSBID();
    if (depSBID.isFree)
    {
        m_errorHandler.reportError((int)dep.getInstGlobalID(), "SBID SHOULDN'T BE FREE!");
    }
    // clears all the buckets
    clearDepBuckets(dep);

    // In case of shooting down of this instruction, we need to add sync to preserve the swsb id sync,
    // so that it's safe to clear the dep
    if (currInst.hasPredication() ||
        (currInst.getExecSize() != dep.getInstruction()->getExecSize()) ||
        (currInst.getChannelOffset() != dep.getInstruction()->getChannelOffset()))
        needSyncForShootDownInst = true;

    // used to set read or write dependency
    depSBID.dType = dep.getDepType();

    // activeSBID stores all sbid that this inst has dependency on
    // and it'll be processed in processActiveSBID
    bool push_back = true;
    // making sure there are no duplicates
    for (auto& aSBID : activeSBID)
    {
        if (aSBID.sbid == depSBID.sbid)
        {
            //write takes longer then read
            //so we only need to check on one.
            //so this either sets a write or resets back to read
            if (aSBID.dType == DEP_TYPE::READ)
            {
                aSBID.dType = depSBID.dType;
            }
            push_back = false;
            break;
        }
    }
    // adding to active SBID
    // in Run function we will see how many this instruction relies on
    // and generate approriate SWSB and if needed test instruction
    // in that level also will add them back to free list
    if (push_back)
    {
        activeSBID.push_back(depSBID);
    }
}

void SWSBAnalyzer::insertSyncAllRdWr(InstList::iterator insertPoint, Block *bb)
{
    SWSB distanceDependency;
    auto clearRD = m_kernel.createSyncAllRdInstruction(distanceDependency);
    auto clearWR = m_kernel.createSyncAllWrInstruction(distanceDependency);

    if (insertPoint == bb->getInstList().end())
    {
        bb->getInstList().push_back(clearRD);
        bb->getInstList().push_back(clearWR);
    }
    else
    {
        bb->insertInstBefore(insertPoint, clearRD);
        bb->insertInstBefore(insertPoint, clearWR);
    }
}

//TODO this should also clear up grf dependency to handle this case:
/*
call (16|M0)             r8.0:ud          32
sendc.rc (16|M0)         null     r118  null  0x0         0x140B1000 {} //   wr:10h, rd:0, Render Target Write msc:16, to #0
(W)      mov (1|M0)               a0.0<1>:ud    r7.0<0;1,0>:ud
sendc.rc (16|M0)         null     r100  null  0x0         0x140B1000 {} //   wr:10h, rd:0, Render Target Write msc:16, to #0
sendc.rc (16|M0)         null     r118  null  0x0         0x140B1000 {} //   wr:10h, rd:0, Render Target Write msc:16, to #0
(W)      mov (16|M0)               r118.0<1>:ud  r6.0<8;8,1>:ud
(W)      send.dc0 (16|M0)         r38       r118  null  0x0         a0.0
ret (16|M0)

Right now mov will have false dependense on the first send.
*/
void SWSBAnalyzer::clearSBIDDependence(InstList::iterator insertPoint, Instruction *lastInst, Block *bb)
{
    bool sbidInUse = false;
    for (uint32_t i = 0; i < m_SBIDCount; ++i)
    {
        //there are still dependencies that might be used outside of this basic block
        if (!m_freeSBIDList[i].isFree)
        {
            sbidInUse = true;
        }
        m_freeSBIDList[i].reset();
    }

    // if last instruction in basic block is EOT no need to generate flushes
    // hardware will take care of it
    if (lastInst && lastInst->getOpSpec().isSendFamily() && lastInst->hasInstOpt(InstOpt::EOT))
    {
        sbidInUse = false;
    }

    // platform check is mainly for testing purposes
    if (sbidInUse)
    {
        insertSyncAllRdWr(insertPoint, bb);
    }
}

// Keeping track of dependencies that need to be cleared because they are no longer relevant
// right now each BB ends with control flow instruction, and we reset at each BB
void SWSBAnalyzer::clearBuckets(DepSet* input, DepSet* output) {
    if (input->getDepClass() != DEP_CLASS::IN_ORDER)
        return;

    if (m_initPoint) {
        m_distanceTracker.emplace_back(input, output);
        m_initPoint = false;

    }
    else {
        // add DepSet to m_distanceTracker
        m_distanceTracker.emplace_back(input, output);

        auto get_depset_id = [&](DEP_PIPE pipe_type, DepSet& dep_set) {
            if (getNumOfDistPipe() == 1)
                return dep_set.getInstIDs().inOrder;
            return (uint32_t)0;
        };

        auto get_latency = [&](DEP_PIPE pipe_type) {
            return m_LatencyInOrderPipe;
        };

        DEP_PIPE new_pipe = input->getDepPipe();
        // max B2B latency of thie pipe
        size_t max_dis = get_latency(new_pipe);
        // Remove nodes from the Tracker if the latency is already satified
        m_distanceTracker.remove_if(
            [=](const distanceTrackerNode& node) {
                // bypass nodes those are not belong to the same pipe
                if (node.input->getDepPipe() != new_pipe)
                    return false;

                // if the distance >= max_latency, clear buckets for corresponding
                // input and output Dependency
                size_t new_id = get_depset_id(new_pipe, *input);
                if ((new_id - get_depset_id(new_pipe, *node.input)) >= max_dis) {
                    clearDepBuckets(*node.input);
                    clearDepBuckets(*node.output);
                    return true;
                }
                return false;
            }
        );
    }
}

void SWSBAnalyzer::processActiveSBID(SWSB &distanceDependency, const DepSet* input,
    Block *bb, InstList::iterator instIter, vector<SBID>& activeSBID)
{
    // If instruction depends on one or more SBIDS, first one goes in to SWSB field
    // for rest we generate wait instructions.
    for (auto aSBID : activeSBID)
    {
        // Could be we had operation depending on the write
        /*
        *   This case also gets triggered when we have send in BB and dependence in another BB
        *   L0:
        *   call (16|M0)             r8.0          L64
        *   L16:
        *   sendc.rc (16|M0)         null     r118  null  0x0         0x140B1000 {$0} //   wr:10h, rd:0, Render Target Write msc:16, to #0
        *   L64:
        *   (W)      mov (16|M0)              r118.0<1>:ud  r6.0<8;8,1>:ud
        *   (W)      send.dc0 (16|M0)         r38      r118  null  0x0         a0.0       {@1, $0}
        *   ret (16|M0)                          r8.0                             {@3}
        *   After first BB in which sendc.rc ends we clear all SBID and generate sync instructions
        *   On mov it detects dependense, but all SBID are freed.
        */
        if (m_freeSBIDList[aSBID.sbid].isFree)
        {
            continue;
        }

        SWSB::TokenType tType = SWSB::TokenType::NOTOKEN;
        if (aSBID.dType == DEP_TYPE::READ ||
            aSBID.dType == DEP_TYPE::READ_ALWAYS_INTERFERE)
        {
            tType = SWSB::TokenType::SRC;
        }
        else
        {
            tType = SWSB::TokenType::DST;
            //if SBID is cleared add it back to free pool
            //write is last thing. So if instruction depends on it we know read is done
            //but not vice versa
            m_freeSBIDList[aSBID.sbid].reset();
            // clean up the dependency
            assert(m_IdToDepSetMap.find(aSBID.sbid) != m_IdToDepSetMap.end());
            assert(m_IdToDepSetMap[aSBID.sbid].first->getDepClass() == DEP_CLASS::OUT_OF_ORDER);
            clearDepBuckets(*m_IdToDepSetMap[aSBID.sbid].first);
            clearDepBuckets(*m_IdToDepSetMap[aSBID.sbid].second);
        }

        // Setting first SBID as part of instruction
        // If this instruction depends on more SBID, generate sync for the extra ids
        // TODO: Is it safe to clear SBID here?
        if (distanceDependency.tokenType == SWSB::TokenType::NOTOKEN)
        {
            distanceDependency.tokenType = tType;
            distanceDependency.sbid = aSBID.sbid;
        } else {
            // add sync for the id
            SWSB sync_swsb(SWSB::DistType::NO_DIST, tType, 0, aSBID.sbid);
            auto nopInst = m_kernel.createSyncNopInstruction(sync_swsb);
            bb->insertInstBefore(instIter, nopInst);
        }
    }

    // verify if the combination of token and dist is valid, if not, move the
    // token dependency out and add a sync for it
    if (!distanceDependency.verify(m_swsbMode, getInstType(*input->getInstruction()))) {
        // add sync for the id
        SWSB sync_swsb(SWSB::DistType::NO_DIST, distanceDependency.tokenType, 0,
                        distanceDependency.sbid);
        auto nopInst = m_kernel.createSyncNopInstruction(sync_swsb);
        bb->insertInstBefore(instIter, nopInst);
        distanceDependency.tokenType = SWSB::TokenType::NOTOKEN;
        distanceDependency.sbid = 0;
    }
    assert(distanceDependency.verify(m_swsbMode, getInstType(*input->getInstruction())));
}

SWSB::InstType SWSBAnalyzer::getInstType(const Instruction& inst) {
    if (inst.getOpSpec().isSendOrSendsFamily())
        return SWSB::InstType::SEND;
    else if (inst.is(Op::MATH))
        return SWSB::InstType::MATH;
    return SWSB::InstType::OTHERS;
}

uint32_t SWSBAnalyzer::getNumOfDistPipe()
{
    switch(m_swsbMode) {
    case SWSB_ENCODE_MODE::SingleDistPipe:
        return 1;
    default:
        break;
    }
    return 0;
}

void SWSBAnalyzer::advanceInorderInstCounter(DEP_PIPE dep_pipe)
{
    ++m_InstIdCounter.inOrder;

}


static bool isSyncNop(const Instruction &i) {
    return i.is(Op::SYNC) && i.getSyncFc() == SyncFC::NOP;
};

void SWSBAnalyzer::postProcess()
{
    // revisit all instructions to remove redundant sync.nop
    // sync.nop carry the sbid the same as the sbid set on the following instruction can be
    // removed since it'll automatically be sync-ed when sbid is reused. For example:
    // sync.nop        null                       {$0.dst} // can be removed
    // math.exp(8|M0)  r12.0<1>:f  r10.0<8;8,1>:f {$0}
    for (Block* bb : m_kernel.getBlockList())
    {
        InstList& instList = bb->getInstList();
        if (instList.empty())
            continue;
        auto inst_it = instList.begin();
        // skip the first instruction, which must not be sync

        ++inst_it;
        for (; inst_it != instList.end(); ++inst_it)
        {
            Instruction* inst = *inst_it;
            if (isSyncNop(*inst))
                continue;
            SWSB cur_swsb = inst->getSWSB();
            if (cur_swsb.hasToken() && (cur_swsb.tokenType == SWSB::TokenType::SET)) {
                // iterate through the previous sync
                auto sync_it = inst_it;
                --sync_it;
                while (sync_it != instList.begin()) {
                    Instruction* sync_inst = *sync_it;
                    if (!isSyncNop(*sync_inst))
                        break;
                    SWSB sync_swsb = sync_inst->getSWSB();
                    // if the sync has sbid set, it could be the reserved sbid for shoot down
                    // instructions, we should keep it.
                    if (sync_swsb.hasToken() && sync_swsb.tokenType != SWSB::TokenType::SET &&
                        sync_swsb.sbid == cur_swsb.sbid) {
                        // clean the swsb so that we can remove this instruction later
                        sync_inst->setSWSB(SWSB());
                    }
                    --sync_it;
                }
            }
        }
        // remove the redundant sync.nop (sync.nop with no swsb)
        instList.remove_if([](const Instruction* inst) {
            return isSyncNop(*inst) && !inst->getSWSB().hasSWSB();
        });
    }
}

SBID& SWSBAnalyzer::assignSBID(DepSet* input, DepSet* output, Instruction& inst, SWSB& distanceDependency,
    InstList::iterator insertPoint, Block *curBB, bool needSyncForShootDown)
{
    bool foundFree = false;
    SBID *sbidFree = nullptr;
    for (uint32_t i = 0; i < m_SBIDCount; ++i)
    {
        if (m_freeSBIDList[i].isFree)
        {
            foundFree = true;
            sbidFree = &m_freeSBIDList[i];
            m_freeSBIDList[i].sbid = i;
            break;
        }
    }
    // no free SBID.
    if (!foundFree)
    {
        unsigned int index = (m_SBIDRRCounter++) % m_SBIDCount;

        // While swsb id being reuse, the dependency will automatically resolved by hardware,
        // so cleanup the dependency bucket for instruction that previously used this id
        assert(m_IdToDepSetMap.find(index) != m_IdToDepSetMap.end());
        assert(m_IdToDepSetMap[index].first->getDepClass() == DEP_CLASS::OUT_OF_ORDER);
        clearDepBuckets(*m_IdToDepSetMap[index].first);
        clearDepBuckets(*m_IdToDepSetMap[index].second);

        m_freeSBIDList[index].reset();
        sbidFree = &m_freeSBIDList[index];
        sbidFree->sbid = index;
    }
    sbidFree->isFree = false;
    input->setSBID(*sbidFree);
    output->setSBID(*sbidFree);
    if (m_IdToDepSetMap.find(sbidFree->sbid) != m_IdToDepSetMap.end())
        m_IdToDepSetMap.erase(sbidFree->sbid);
    m_IdToDepSetMap.insert(make_pair(sbidFree->sbid, make_pair(input, output)));

    // adding the set for this SBID
    // if the swsb has the token set already, move it out to a sync
    if (distanceDependency.tokenType != SWSB::TokenType::NOTOKEN) {
        SWSB tDep(SWSB::DistType::NO_DIST, distanceDependency.tokenType,
            0, distanceDependency.sbid);
        Instruction* tInst = m_kernel.createSyncNopInstruction(tDep);
        curBB->insertInstBefore(insertPoint, tInst);
    }
    // set the sbid
    distanceDependency.tokenType = SWSB::TokenType::SET;
    distanceDependency.sbid = sbidFree->sbid;

    // verify if the token and dist combination is valid, if not, move the dist out to a sync
    // FIXME: move the dist out here to let the sbid set on the instruction could have better readability
    // but a potential issue is that A@1 is required to be set on the instruction having
    // architecture read/write. This case A@1 will be moved out from the instruction
    if (!distanceDependency.verify(m_swsbMode, getInstType(inst))) {
        SWSB tDep(distanceDependency.distType, SWSB::TokenType::NOTOKEN,
            distanceDependency.minDist, 0);
        Instruction* tInst = m_kernel.createSyncNopInstruction(tDep);
        curBB->insertInstBefore(insertPoint, tInst);
        distanceDependency.distType = SWSB::DistType::NO_DIST;
        distanceDependency.minDist = 0;
    }
    assert(distanceDependency.verify(m_swsbMode, getInstType(inst)));

    // add a sync to preserve the token for possibly shooting down instruction
    if (needSyncForShootDown) {
        SWSB tDep(SWSB::DistType::NO_DIST, distanceDependency.tokenType,
            0, distanceDependency.sbid);
        Instruction* tInst = m_kernel.createSyncNopInstruction(tDep);
        curBB->insertInstBefore(insertPoint, tInst);
    }

    assert(sbidFree != nullptr);
    return *sbidFree;
}

void SWSBAnalyzer::run()
{
    m_initPoint = true;
    m_distanceTracker.clear();

    for (uint32_t i = 0; i < MAX_GRF_BUCKETS; ++i)
    {
        m_buckets[i].clearDependency();
    }

    // init in order pipe id counters
    m_InstIdCounter.inOrder = 1;

    // init the math WA struct
    // When there is a math instruction, when the following instruction has different
    // predication to the math, should assume the math taking the entire GRF in it's
    // dst no matter the access region and channels are.
    struct MathWAInfo {
        bool previous_is_math = false;
        DepSet* dep_set = nullptr;
        // a special id to identify this DepSet when trying to clean it from buckets
        const InstIDs math_id = {std::numeric_limits<uint32_t>::max(), 0};
        Instruction* math_inst = nullptr;
        SBID math_sbid = {0, true, DEP_TYPE::NONE};

        void reset() {
            previous_is_math = false;
            dep_set = nullptr;
            math_inst = nullptr;
            math_sbid = {0, true, DEP_TYPE::NONE};
        }
    } math_wa_info;

    Instruction* inst = nullptr;
    Block * lastBB = nullptr;
    for (auto bb : m_kernel.getBlockList())
    {
        bool blockEndsWithNonBranchInst = false;
        // resetting things for each bb
        lastBB = bb;
        InstList& instList  = bb->getInstList(); // Don't use auto for over loaded return which has const...
        const auto instListEnd    = instList.end();
        for (auto instIter = instList.begin(); instIter != instListEnd; ++instIter)
        {
            m_InstIdCounter.global++;
            inst = *instIter;
            DepSet* input = nullptr;
            DepSet* output = nullptr;

            if (math_wa_info.math_inst != nullptr)
                math_wa_info.previous_is_math = true;
            if (inst->getOpSpec().is(Op::MATH)) {
                math_wa_info.math_inst = inst;

                // if the math following a math, we only care about the last math
                math_wa_info.previous_is_math = false;
            }

                input = m_DB->createSrcDepSet(*inst, m_InstIdCounter, m_swsbMode);
                output = m_DB->createDstDepSet(*inst, m_InstIdCounter, m_swsbMode);
            input->setCompanion(output);
            output->setCompanion(input);


            SWSB distanceDependency;

            // Either source or destination are indirect, or there are SR access,
            // We don't know what registers are being accessed
            // Need to flush all the sbids and set distance to 1
            if (input->hasIndirect() || output->hasIndirect() ||
                input->hasSR() || output->hasSR())
            {
                // clear out-of-order dependency, insert sync.allrd and sync.allwr
                // if there are un-resolved sbid dependecny
                // if this instruction itself is an out-of-order instruction, insert
                // sync.all anyway.
                InstListIterator insert_point = instIter;
                if (input->getDepClass() == DEP_CLASS::OUT_OF_ORDER)
                    insertSyncAllRdWr(insert_point, bb);
                else
                    clearSBIDDependence(insert_point, inst, bb);

                // clear in-order dependency
                clearBuckets(input, output);

                // will add direct accesses to buckets
                // adding dependencies to buckets
                for (auto bucketID : input->getBuckets())
                {
                    m_buckets[bucketID].addDepSet(input);
                }
                for (auto bucketID : output->getBuckets())
                {
                    m_buckets[bucketID].addDepSet(output);
                }

                // set to check all dist pipes
                if (getNumOfDistPipe() == 1)
                    distanceDependency.distType = SWSB::REG_DIST;

                distanceDependency.minDist = 1;
                // input and output must have the same dep class and in the same pipe
                // so check the input only to add the instCounter
                // FIXME: is it possilbe that a instruction has output and no input?
                if (input->getDepClass() == DEP_CLASS::IN_ORDER)
                    advanceInorderInstCounter(input->getDepPipe());

                // if this is an out-of-order instruction, we still need to assign an sbid for it
                if (output->getDepClass() == DEP_CLASS::OUT_OF_ORDER)
                    assignSBID(input, output, *inst, distanceDependency, insert_point, bb, false);

                inst->setSWSB(distanceDependency);
                // clean up math_wa_info, this instruction force to sync all, no need to consider
                // math wa
                if (math_wa_info.previous_is_math) {
                    math_wa_info.reset();
                }
                // early out, no need to calculateDependenc that all dependencies are resolved.
                continue;
            } // end indirect access handling

            if (math_wa_info.previous_is_math) {
                // math WA affect the instruction right after the math, and with different predication
                // Add the WA math dst region to Buckets
                if (math_wa_info.math_inst->getPredication().function != inst->getPredication().function) {
                    math_wa_info.dep_set =
                        m_DB->createMathDstWADepSet(*math_wa_info.math_inst, math_wa_info.math_id, m_swsbMode);
                    math_wa_info.dep_set->setSBID(math_wa_info.math_sbid);
                    for (auto bucketID : math_wa_info.dep_set->getBuckets())
                    {
                        IGA_ASSERT(bucketID < m_DB->getTOTAL_BUCKETS(), "buckedID out of range");
                        m_buckets[bucketID].addDepSet(math_wa_info.dep_set);
                    }
                }
            }

            vector<SBID> activeSBID;
            bool needSyncForShootDown = false;
            // Calculates dependence between this instruction dependencies and previous ones.
            calculateDependence(*input, distanceDependency, *inst, activeSBID, needSyncForShootDown);
            calculateDependence(*output, distanceDependency, *inst, activeSBID, needSyncForShootDown);

            // clean up math_wa_info
            if (math_wa_info.previous_is_math) {
                if (math_wa_info.dep_set != nullptr)
                    clearDepBuckets(*math_wa_info.dep_set);
                math_wa_info.reset();
            }

                processActiveSBID(distanceDependency, input, bb, instIter, activeSBID);

            // Need to set SBID
            if (output->getDepClass() == DEP_CLASS::OUT_OF_ORDER &&
                !(inst->getOpSpec().isSendFamily() && inst->hasInstOpt(InstOpt::EOT)))
            {
                InstList::iterator insertPoint = instIter;
                SBID& assigned_id = assignSBID(input, output, *inst, distanceDependency,
                    insertPoint, bb, needSyncForShootDown);

                // record the sbid if it's math, for use of math wa
                if (inst->getOpSpec().is(Op::MATH)) {
                    math_wa_info.math_sbid = assigned_id;
                }
            }

            clearBuckets(input, output);

            /*
             * Handling the case where everything is in one bb, and send with EOT is in the middle of instruction stream
             *           call (16|M0)             r8.0:ud          32
             *           sendc.rc (16|M0)         null     r118  null  0x0         0x140B1000 {EOT} //   wr:10h, rd:0, Render Target Write msc:16, to #0
             *           ...
             *           ret (16|M0)                          r8.0
             */
            if (!(inst->getOpSpec().isSendFamily() && inst->hasInstOpt(InstOpt::EOT)))
            {
                //adding dependencies to buckets
                for (auto bucketID : input->getBuckets())
                {
                    // We want to check dependncy of regular instructions against
                    // WRITE_ALWAYS_INTERFERE without adding them themselves
                    if (bucketID == m_DB->getBucketStart(RegName::ARF_CR) &&
                        input->getDepType() != DEP_TYPE::WRITE_ALWAYS_INTERFERE &&
                        input->getDepType() != DEP_TYPE::READ_ALWAYS_INTERFERE)
                    {
                        continue;
                    }
                    m_buckets[bucketID].addDepSet(input);
                }
                for (auto bucketID : output->getBuckets())
                {
                    IGA_ASSERT(bucketID < m_DB->getTOTAL_BUCKETS(),
                        "buckedID out of range");
                    // We want to check dependncy of regular instructions against
                    // WRITE_ALWAYS_INTERFERE without adding them themselves
                    if (bucketID == m_DB->getBucketStart(RegName::ARF_CR) &&
                        output->getDepType() != DEP_TYPE::WRITE_ALWAYS_INTERFERE &&
                        output->getDepType() != DEP_TYPE::READ_ALWAYS_INTERFERE)
                    {
                        continue;
                    }
                    m_buckets[bucketID].addDepSet(output);
                }
            }

            if (input->getDepClass() == DEP_CLASS::IN_ORDER)
            {
                advanceInorderInstCounter(input->getDepPipe());
            }

                inst->setSWSB(distanceDependency);

            assert(distanceDependency.verify(m_swsbMode, getInstType(*inst)));

            if (inst->isBranching())
            {
                //TODO: konrad : this is somewhat conservative, some
                //branch instructions might not need sync (join)
                blockEndsWithNonBranchInst = false;
                clearSBIDDependence(instIter, inst, bb);
                continue;
            }
            else
            {
                blockEndsWithNonBranchInst = true;
            }
        } //iterate on instr
        //          clear read
        //          clear write
        if (blockEndsWithNonBranchInst) {
            clearSBIDDependence(instList.end(), inst, bb);
        }
    } //iterate on basic block

    // this code is for FC composite
    // if last instruction is not EOT we will insert flush instructions
    // and stall the pipeline since we do not do global analysis
    if (inst &&
        ((inst->getOpSpec().isSendFamily() &&
            !inst->getInstOpts().contains(InstOpt::EOT)) || !inst->getOpSpec().isSendFamily()))
    {
        SWSB swsb;
        if (getNumOfDistPipe() == 1)
            swsb.distType = SWSB::REG_DIST;
        swsb.minDist = 1;
        Instruction *syncInst = m_kernel.createSyncNopInstruction(swsb);
        lastBB->getInstList().push_back(syncInst);
    }

    postProcess();
    return;
}
