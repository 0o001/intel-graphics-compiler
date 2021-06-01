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

#include "LoopAnalysis.h"
#include "G4_Kernel.hpp"
#include "G4_BB.hpp"

using namespace vISA;

G4_BB* Dominator::InterSect(G4_BB* bb, int i, int k)
{
    recomputeIfStale();

    G4_BB* finger1 = immDoms[bb->getId()][i];
    G4_BB* finger2 = immDoms[bb->getId()][k];

    while ((finger1 != finger2) &&
        (finger1 != nullptr) &&
        (finger2 != nullptr))
    {
        if (finger1->getPreId() == finger2->getPreId())
        {
            assert(finger1 == kernel.fg.getEntryBB() || finger2 == kernel.fg.getEntryBB());
            return kernel.fg.getEntryBB();
        }

        while ((iDoms[finger1->getId()] != nullptr) &&
            (finger1->getPreId() > finger2->getPreId()))
        {
            finger1 = iDoms[finger1->getId()];
            immDoms[bb->getId()][i] = finger1;
        }

        while ((iDoms[finger2->getId()] != nullptr) &&
            (finger2->getPreId() > finger1->getPreId()))
        {
            finger2 = iDoms[finger2->getId()];
            immDoms[bb->getId()][k] = finger2;
        }

        if ((iDoms[finger2->getId()] == nullptr) ||
            (iDoms[finger1->getId()] == nullptr))
        {
            break;
        }
    }

    if (finger1 == finger2)
    {
        return finger1;
    }
    else if (finger1->getPreId() > finger2->getPreId())
    {
        return finger2;
    }
    else
    {
        return finger1;
    }
}

/*
* An improvement on the algorithm from "A Simple, Fast Dominance Algorithm"
* 1. Single pred assginment.
* 2. To reduce the back trace in the intersect function, a temp buffer for predictor of each nodes is used to record the back trace result.
*/
void Dominator::runIDOM()
{
    iDoms.resize(kernel.fg.size());
    immDoms.resize(kernel.fg.size());

    for (auto I = kernel.fg.cbegin(), E = kernel.fg.cend(); I != E; ++I)
    {
        auto bb = *I;
        iDoms[bb->getId()] = nullptr;
        immDoms[bb->getId()].resize(bb->Preds.size());

        size_t i = 0;
        for (auto pred : bb->Preds)
        {
            immDoms[bb->getId()][i] = pred;
            i++;
        }
    }

    entryBB = kernel.fg.getEntryBB();
    iDoms[entryBB->getId()] = { entryBB };

    // Actual dom computation
    bool change = true;
    while (change)
    {
        change = false;
        for (auto I = kernel.fg.cbegin(), E = kernel.fg.cend(); I != E; ++I)
        {
            auto bb = *I;
            if (bb == entryBB)
                continue;

            if (bb->Preds.size() == 1)
            {
                if (iDoms[bb->getId()] == nullptr)
                {
                    iDoms[bb->getId()] = (*bb->Preds.begin());
                    change = true;
                }
                else
                {
                    assert(iDoms[bb->getId()] == (*bb->Preds.begin()));
                }
            }
            else
            {
                G4_BB* tmpIdom = nullptr;
                int i = 0;
                for (auto pred : bb->Preds)
                {
                    if (iDoms[pred->getId()] != nullptr)
                    {
                        tmpIdom = pred;
                        break;
                    }
                    i++;
                }

                if (tmpIdom != nullptr)
                {
                    int k = 0;
                    for (auto pred : bb->Preds)
                    {
                        if (k == i)
                        {
                            k++;
                            continue;
                        }

                        if (iDoms[pred->getId()] != nullptr)
                        {
                            tmpIdom = InterSect(bb, i, k);
                        }
                        k++;
                    }

                    if (iDoms[bb->getId()] == nullptr ||
                        iDoms[bb->getId()] != tmpIdom)
                    {
                        iDoms[bb->getId()] = tmpIdom;
                        change = true;
                    }
                }
            }
        }
    }
}

void Dominator::runDOM()
{
    Doms.resize(kernel.fg.size());
    entryBB = kernel.fg.getEntryBB();

    MUST_BE_TRUE(entryBB != nullptr, "Entry BB not found!");

    Doms[entryBB->getId()] = { entryBB };
    std::unordered_set<G4_BB*> allBBs;
    for (auto I = kernel.fg.cbegin(), E = kernel.fg.cend(); I != E; ++I)
    {
        auto bb = *I;
        allBBs.insert(bb);
    }

    for (auto I = kernel.fg.cbegin(), E = kernel.fg.cend(); I != E; ++I)
    {
        auto bb = *I;
        if (bb != entryBB)
        {
            Doms[bb->getId()] = allBBs;
        }
    }

    // Actual dom computation
    bool change = true;
    while (change)
    {
        change = false;
        for (auto I = kernel.fg.cbegin(), E = kernel.fg.cend(); I != E; ++I)
        {
            auto bb = *I;
            if (bb == entryBB)
                continue;

            std::unordered_set<G4_BB*> tmp = { bb };

            // Compute intersection of dom of preds
            std::unordered_map<G4_BB*, unsigned int> numInstances;

            //
            for (auto preds : bb->Preds)
            {
                auto& domPred = Doms[preds->getId()];
                for (auto domPredBB : domPred)
                {
                    auto it = numInstances.find(domPredBB);
                    if (it == numInstances.end())  //Not found
                        numInstances.insert(std::make_pair(domPredBB, 1));
                    else
                        it->second = it->second + 1;
                }
            }

            // Common BBs appear in numInstances map with second value == bb->Preds count
            for (auto commonBBs : numInstances)
            {
                if (commonBBs.second == bb->Preds.size()) //same size means the bb from all preds.
                    tmp.insert(commonBBs.first);
            }

            // Check if Dom set changed for bb in current iter
            if (tmp.size() != Doms[bb->getId()].size())  //Same size
            {
                Doms[bb->getId()] = tmp;
                change = true;
                continue;
            }
            else //Same
            {
                auto& domBB = Doms[bb->getId()];
                for (auto tmpBB : tmp)
                {
                    if (domBB.find(tmpBB) == domBB.end()) //Same BB
                    {
                        Doms[bb->getId()] = tmp;
                        change = true;
                        break;
                    }
                    if (change)
                        break;
                }
            }
        }
    }

    updateImmDom();
}


std::unordered_set<G4_BB*>& Dominator::getDom(G4_BB* bb)
{
    recomputeIfStale();

    return Doms[bb->getId()];
}

std::vector<G4_BB*>& Dominator::getImmDom(G4_BB* bb)
{
    recomputeIfStale();

    return immDoms[bb->getId()];
}

void Dominator::updateImmDom()
{
    std::vector<BitSet> domBits(kernel.fg.size());

    for (size_t i = 0; i < kernel.fg.size(); i++)
    {
        domBits[i] = BitSet(unsigned(kernel.fg.size()), false);
    }

    // Update immDom vector with correct ordering
    for (auto bb : kernel.fg)
    {
        auto& DomBBs = Doms[bb->getId()];

        for (auto domBB : DomBBs)
        {
            domBits[bb->getId()].set(domBB->getId(), true);
        }
    }

    iDoms.resize(kernel.fg.size());
    for (auto bb : kernel.fg)
    {
        auto& DomBBs = Doms[bb->getId()];
        BitSet tmpBits = domBits[bb->getId()];
        tmpBits.set(bb->getId(), false);
        iDoms[bb->getId()] = bb;

        for (auto domBB : DomBBs)
        {
            if (domBB == bb)
                continue;

            if (tmpBits == domBits[domBB->getId()])
            {
                iDoms[bb->getId()] = domBB;
            }
        }
    }
}

void Dominator::reset()
{
    iDoms.clear();
    Doms.clear();
    immDoms.clear();

    setStale();
}

void Dominator::run()
{
    // this function re-runs analysis. caller needs to check if
    // analysis is stale.
    entryBB = kernel.fg.getEntryBB();

    runDOM();
    runIDOM();

    setValid();
}

void Dominator::dump(std::ostream& os)
{
    if (isStale())
        os << "Dominator data is stale.\n";

    os << "Dom:\n";
    dumpDom(os);

    os << "\n\nImm dom:\n";
    dumpImmDom(os);
}

const std::vector<G4_BB*>& Dominator::getIDoms()
{
    recomputeIfStale();

    return iDoms;
}

G4_BB* Dominator::getCommonImmDom(const std::unordered_set<G4_BB*>& bbs)
{
    recomputeIfStale();

    if (bbs.size() == 0)
        return nullptr;

    unsigned int maxId = (*bbs.begin())->getId();

    auto commonImmDoms = getImmDom(*bbs.begin());
    for (auto bb : bbs)
    {
        maxId = std::max(maxId, bb->getId());

        const auto& DomBB = Doms[bb->getId()];
        for (G4_BB*& dom : commonImmDoms)
        {
            if (dom != nullptr && DomBB.find(dom) == DomBB.end())
            {
                dom = nullptr;
            }
        }
    }

    // Return first imm dom that is not a BB from bbs set
    for (G4_BB* dom : commonImmDoms)
    {
        if (dom &&
            // Common imm pdom must be lexically last BB
            dom->getId() >= maxId &&
            ((dom->size() > 1 && dom->front()->isLabel()) ||
                (dom->size() > 0 && !dom->front()->isLabel())))
        {
            return dom;
        }
    }

    return entryBB;
}

void Dominator::dumpImmDom(std::ostream& os)
{
    for (auto bb : kernel.fg)
    {
        os << "BB" << bb->getId() << " - ";
        auto& domBBs = immDoms[bb->getId()];
        for (auto domBB : domBBs)
        {
            os << "BB" << domBB->getId();
            if (domBB->getLabel())
            {
                os << " (" << domBB->getLabel()->getLabel() << ")";
            }
            os << ", ";
        }
        os << "\n";
    }
}

void vISA::Dominator::dumpDom(std::ostream& os)
{
    for (auto bb : kernel.fg)
    {
        os << "BB" << bb->getId() << " - ";
        auto& domBBs = Doms[bb->getId()];
        for (auto domBB : domBBs)
        {
            os << "BB" << domBB->getId();
            if (domBB->getLabel())
            {
                os << " (" << domBB->getLabel()->getLabel() << ")";
            }
            os << ", ";
        }
        os << "\n";
    }
}

// return true if bb1 dominates bb2
bool Dominator::dominates(G4_BB* bb1, G4_BB* bb2)
{
    recomputeIfStale();

    auto& dom = getDom(bb1);
    if (dom.find(bb2) != dom.end())
        return true;

    return false;
}

void Analysis::recomputeIfStale()
{
    if (!isStale() || inProgress)
        return;

    inProgress = true;
    reset();
    run();
    inProgress = false;
}

PostDom::PostDom(G4_Kernel& k) : kernel(k)
{
}

void PostDom::reset()
{
    postDoms.clear();
    immPostDoms.clear();

    setStale();
}

void PostDom::run()
{
    exitBB = nullptr;
    auto numBBs = kernel.fg.size();
    postDoms.resize(numBBs);
    immPostDoms.resize(numBBs);

    for (auto bb_rit = kernel.fg.rbegin(); bb_rit != kernel.fg.rend(); bb_rit++)
    {
        auto bb = *bb_rit;
        if (bb->size() > 0)
        {
            auto lastInst = bb->back();
            if (lastInst->isEOT())
            {
                exitBB = bb;
                break;
            }
        }
    }

    MUST_BE_TRUE(exitBB != nullptr, "Exit BB not found!");

    postDoms[exitBB->getId()] = { exitBB };
    std::unordered_set<G4_BB*> allBBs(kernel.fg.cbegin(), kernel.fg.cend());

    for (auto bb : kernel.fg)
    {
        if (bb != exitBB)
        {
            postDoms[bb->getId()] = allBBs;
        }
    }

    // Actual post dom computation
    bool change = true;
    while (change)
    {
        change = false;
        for (auto bb : kernel.fg)
        {
            if (bb == exitBB)
                continue;

            std::unordered_set<G4_BB*> tmp = { bb };
            // Compute intersection of pdom of successors
            std::unordered_map<G4_BB*, unsigned> numInstances;
            for (auto succs : bb->Succs)
            {
                auto& pdomSucc = postDoms[succs->getId()];
                for (auto pdomSuccBB : pdomSucc)
                {
                    auto it = numInstances.find(pdomSuccBB);
                    if (it == numInstances.end())
                        numInstances.insert(std::make_pair(pdomSuccBB, 1));
                    else
                        it->second = it->second + 1;
                }
            }

            // Common BBs appear in numInstances map with second value == bb->Succs count
            for (auto commonBBs : numInstances)
            {
                if (commonBBs.second == bb->Succs.size())
                    tmp.insert(commonBBs.first);
            }

            // Check if postDom set changed for bb in current iter
            if (tmp.size() != postDoms[bb->getId()].size())
            {
                postDoms[bb->getId()] = tmp;
                change = true;
                continue;
            }
            else
            {
                auto& pdomBB = postDoms[bb->getId()];
                for (auto tmpBB : tmp)
                {
                    if (pdomBB.find(tmpBB) == pdomBB.end())
                    {
                        postDoms[bb->getId()] = tmp;
                        change = true;
                        break;
                    }
                    if (change)
                        break;
                }
            }
        }
    }

    setValid();
    updateImmPostDom();
}

std::unordered_set<G4_BB*>& PostDom::getPostDom(G4_BB* bb)
{
    recomputeIfStale();

    return postDoms[bb->getId()];
}

void PostDom::dumpImmDom(std::ostream& os)
{
    if (isStale())
        os << "PostDom data is stale.\n";

    for (auto bb : kernel.fg)
    {
        os << "BB" << bb->getId();
        auto& pdomBBs = immPostDoms[bb->getId()];
        for (auto pdomBB : pdomBBs)
        {
            os << "BB" << pdomBB->getId();
            if (pdomBB->getLabel())
            {
                os << " (" << pdomBB->getLabel()->getLabel() << ")";
            }
            os << ", ";
        }
        os << "\n";
    }
}

std::vector<G4_BB*>& PostDom::getImmPostDom(G4_BB* bb)
{
    recomputeIfStale();

    return immPostDoms[bb->getId()];
}

void PostDom::updateImmPostDom()
{
    // Update immPostDom vector with correct ordering
    for (auto bb : kernel.fg)
    {
        auto& postDomBBs = postDoms[bb->getId()];
        auto& immPostDomBB = immPostDoms[bb->getId()];
        immPostDomBB.resize(postDomBBs.size());
        immPostDomBB[0] = bb;

        for (auto pdomBB : postDomBBs)
        {
            if (pdomBB == bb)
                continue;

            immPostDomBB[postDomBBs.size() - postDoms[pdomBB->getId()].size()] = pdomBB;
        }
    }
}

G4_BB* PostDom::getCommonImmDom(std::unordered_set<G4_BB*>& bbs)
{
    recomputeIfStale();

    if (bbs.size() == 0)
        return nullptr;

    unsigned maxId = (*bbs.begin())->getId();

    auto commonImmDoms = getImmPostDom(*bbs.begin());
    for (auto bb : bbs)
    {
        if (bb->getId() > maxId)
            maxId = bb->getId();

        auto& postDomBB = postDoms[bb->getId()];
        for (unsigned i = 0, size = commonImmDoms.size(); i != size; i++)
        {
            if (commonImmDoms[i])
            {
                if (postDomBB.find(commonImmDoms[i]) == postDomBB.end())
                {
                    commonImmDoms[i] = nullptr;
                }
            }
        }
    }

    // Return first imm dom that is not a BB from bbs set
    for (G4_BB* commonImmDom : commonImmDoms)
    {
        if (commonImmDom &&
            // Common imm pdom must be lexically last BB
            commonImmDom->getId() >= maxId &&
            ((commonImmDom->size() > 1 && commonImmDom->front()->isLabel()) ||
                (commonImmDom->size() > 0 && !commonImmDom->front()->isLabel())))
        {
            return commonImmDom;
        }
    }

    return exitBB;
}

LoopDetection::LoopDetection(G4_Kernel& k) : kernel(k), fg(k.fg)
{
}

std::vector<Loop*> vISA::LoopDetection::getTopLoops()
{
    recomputeIfStale();

    return topLoops;
}

void LoopDetection::reset()
{
    allLoops.clear();
    topLoops.clear();

    PreIdRPostId.clear();

    setStale();
}

// Adapted from FlowGraph::DFSTraverse.
// No changes are made to any G4_BB member or to FlowGraph.
void LoopDetection::DFSTraverse(const G4_BB* startBB, unsigned& preId, unsigned& postId, BackEdges& bes)
{
    std::stack<const G4_BB*> traversalStack;
    traversalStack.push(startBB);

    auto getPreId = [&](const G4_BB* bb)
    {
        return PreIdRPostId[bb].first;
    };

    auto getRPostId = [&](const G4_BB* bb)
    {
        return PreIdRPostId[bb].second;
    };

    auto setPreId = [&](const G4_BB* bb, unsigned int id)
    {
        PreIdRPostId[bb].first = id;
    };

    auto setRPostId = [&](const G4_BB* bb, unsigned int id)
    {
        PreIdRPostId[bb].second = id;
    };

    while (!traversalStack.empty())
    {
        auto bb = traversalStack.top();
        if (getPreId(bb) != UINT_MAX)
        {
            // Pre-processed already and continue to the next one.
            // Before doing so, set postId if not set before.
            traversalStack.pop();
            if (getRPostId(bb) == UINT_MAX)
            {
            // All bb's succ has been visited (PreId is set) at this time.
            // if any of its succ has not been finished (RPostId not set),
            // bb->succ forms a backedge.
            //
            // Note: originally, CALL and EXIT will not check back-edges, here
            //       we skip checking for them as well. (INIT & RETURN should
            //       be checked as well ?)
            if (!(bb->getBBType() & (G4_BB_CALL_TYPE | G4_BB_EXIT_TYPE)))
            {
                for (auto succBB : bb->Succs)
                {
                    if (getRPostId(succBB) == UINT_MAX)
                    {
                        BackEdge be = std::make_pair(const_cast<G4_BB*>(bb), succBB);
                        bes.push_back(be);
                    }
                }
            }

            // Need to keep this after backedge checking so that self-backedge
            // (single-bb loop) will not be missed.
            setRPostId(bb, postId++);
            }
            continue;
        }

        setPreId(bb, preId++);

        if (bb->getBBType() & G4_BB_CALL_TYPE)
        {
            const G4_BB* returnBB = bb->BBAfterCall();

            if (getPreId(returnBB) == UINT_MAX)
            {
                traversalStack.push(returnBB);
            }
            else
            {
                MUST_BE_TRUE(false, ERROR_FLOWGRAPH);
            }
        }
        else if (bb->getBBType() & G4_BB_EXIT_TYPE)
        {
            // Skip
        }
        else
        {
            // To be consistent with previous behavior, use reverse_iter.
            auto RIE = bb->Succs.rend();
            for (auto rit = bb->Succs.rbegin(); rit != RIE; ++rit)
            {
                const G4_BB* succBB = *rit;
                if (getPreId(succBB) == UINT_MAX)
                {
                    traversalStack.push(succBB);
                }
            }
        }
    }
}

void LoopDetection::findDominatingBackEdges(BackEdges& bes)
{
    const auto& BBs = fg.getBBList();

    for (auto& bb : BBs)
    {
        PreIdRPostId[bb] = std::make_pair(UINT_MAX, UINT_MAX);
    }

    unsigned preId = 0;
    unsigned postID = 0;

    DFSTraverse(fg.getEntryBB(), preId, postID, bes);

    for (auto fn : fg.funcInfoTable)
    {
        DFSTraverse(fn->getInitBB(), preId, postID, bes);
    }
}

void LoopDetection::populateLoop(BackEdge& backEdge)
{
    // check whether dst dominates src
    auto src = const_cast<G4_BB*>(backEdge.first);
    auto dst = const_cast<G4_BB*>(backEdge.second);

    auto& domSecond = fg.getDominator().getDom(src);
    if (domSecond.find(dst) != domSecond.end())
    {
        // this is a natural loop back edge. populate all bbs in loop.
        Loop newLoop(backEdge);
        newLoop.id = allLoops.size() + 1;

        newLoop.addBBToLoop(src);
        newLoop.addBBToLoop(dst);

        std::stack<G4_BB*> traversal;
        traversal.push(src);
        while (!traversal.empty())
        {
            auto bb = traversal.top();
            traversal.pop();

            // check whether bb's preds are dominated by loop header.
            // if yes, add them to traversal.
            for (auto predIt = bb->Preds.begin(); predIt != bb->Preds.end(); ++predIt)
            {
                auto pred = (*predIt);
                // pred is loop header, which is already added to list of loop BBs
                if (pred == dst)
                    continue;

                if (fg.getDominator().dominates(pred, dst) &&
                    !newLoop.contains(pred))
                {
                    // pred is part of loop
                    newLoop.addBBToLoop(pred);
                    traversal.push(pred);
                }
            }
        }

        allLoops.emplace_back(newLoop);
    }
}

void LoopDetection::computeLoopTree()
{
    // create loop tree by iterating over allLoops in descending order
    // of BB count.
    std::vector<Loop*> sortedLoops;
    std::for_each(allLoops.begin(), allLoops.end(), [&](Loop& l) { sortedLoops.push_back(&l); });

    // sorting loops by size of contained BBs makes it easy to create
    // tree structure relationship of loops.
    // 1. If loop A has more BBs than loop B then A is either some parent of B or no relationship exists.
    // 2. For loop A to be a parent of loop B, all BBs of loop B have to be contained in loop A as well.
    //
    // processing loops in sorted order of BB size guarantees that we'll create tree in top-down order.
    // we'll never encounter a situation where new loop to be added to tree is parent of an existing
    // loop already present in the tree.
    std::sort(sortedLoops.begin(), sortedLoops.end(), [](Loop* l1, Loop* l2) { return l2->getBBSize() < l1->getBBSize(); });

    for (auto loop : sortedLoops)
    {
        addLoop(loop, nullptr);
    }
}

void LoopDetection::addLoop(Loop* newLoop, Loop* aParent)
{
    if (topLoops.size() == 0)
    {
        topLoops.push_back(newLoop);
        return;
    }

    // find a place in existing loop tree to insert new loop passed in.
    // following scenarios exist:
    // a. loop is nested loop of an existing loop,
    // b. loop is not nested but is sibling of existing loop,
    // c. loop is top level parent loop of a certain tree

    // check if newLoop fits in to any existing current top level loop
    auto siblings = aParent ? aParent->getAllSiblings(topLoops) : topLoops;
    for (auto& sibling : siblings)
    {
        if (newLoop->fullSubset(sibling))
        {
            if (sibling->immNested.size() > 0)
            {
                addLoop(newLoop, sibling->immNested[0]);
            }
            else
            {
                sibling->immNested.push_back(newLoop);
                newLoop->parent = sibling;
            }
            return;
        }
        else if (newLoop->fullSuperset(sibling))
        {
            MUST_BE_TRUE(false, "Not expecting to see parent loop here");
            return;
        }
    }

    // add new sibling to current level
    newLoop->parent = siblings[0]->parent;
    if (newLoop->parent)
    {
        siblings[0]->parent->immNested.push_back(newLoop);
    }
    else
    {
        topLoops.push_back(newLoop);
    }
}

void LoopDetection::run()
{
    BackEdges backEdges;
    findDominatingBackEdges(backEdges);
    for (auto& be : backEdges)
    {
        populateLoop(be);
    }

    computeLoopTree();

    setValid();
}

void LoopDetection::dump(std::ostream& os)
{
    if(isStale())
        os << "Loop info is stale.\n";

    os << "\n\n\nLoop tree:\n";

    for (auto loop : topLoops)
    {
        loop->dump();
    }
}

// add bb to current loop and to all valid parents
void Loop::addBBToLoopHierarchy(G4_BB* bb)
{
    addBBToLoop(bb);

    if (parent)
        parent->addBBToLoopHierarchy(bb);
}

void vISA::Loop::addBBToLoop(G4_BB* bb)
{
    BBs.push_back(bb);
    BBsLookup.insert(bb);
}

bool Loop::fullSubset(Loop* other)
{
    if (BBs.size() > other->BBs.size())
        return false;

    // to avoid O(N^2) lookup, use unordered set of other loop's BBs for lookup
    auto& otherBBs = other->BBsLookup;

    // check whether current loop's all BBs are fully contained in "other" loop
    for (auto bb : BBs)
    {
        if (otherBBs.find(bb) == otherBBs.end())
            return false;
    }

    return true;
}

bool Loop::fullSuperset(Loop* other)
{
    return other->fullSubset(this);
}

std::vector<Loop*> Loop::getAllSiblings(std::vector<Loop*>& topLoops)
{
    if (parent)
        return parent->immNested;

    return topLoops;
}

unsigned int Loop::getNestingLevel()
{
    if (!parent)
        return 1;

    return parent->getNestingLevel()+1;
}

void Loop::dump(std::ostream& os)
{
    auto nestingLevel = getNestingLevel();
    nestingLevel = nestingLevel > 0 ? nestingLevel : 1;
    for (unsigned int i = 0; i != nestingLevel - 1; ++i)
    {
        os << "\t";
    }
    os << "L" << id << ": - { ";
    for (auto bb : BBs)
    {
        os << bb->getId();
        if (bb != BBs.back())
            os << ", ";
    }
    os << " } ";

    os << " BE: {BB" << be.first->getId() << " -> BB" << be.second->getId() << "}\n";

    for (auto& nested : immNested)
    {
        nested->dump();
    }
}

bool vISA::Loop::contains(const G4_BB* bb)
{
    return BBsLookup.find(bb) != BBsLookup.end();
}
