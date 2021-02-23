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
#ifndef _G4_LVN_H_
#define _G4_LVN_H_

#include "Optimizer.h"
#include "G4_Opcode.h"
#include "G4_Verifier.hpp"

typedef uint64_t Value_Hash;
namespace vISA
{
    class LVNItemInfo;
    class Value
    {
    public:
        Value_Hash hash = 0;
        G4_INST* inst = nullptr;

        void initializeEmptyValue() { hash = 0; inst = nullptr; }
        bool isValueEmpty() const
        {
            return !inst;
        }
        bool isEqualValueHash(Value& val2) const
        {
            return (hash == val2.hash);
        }
        bool operator==(const Value& other) const
        {
            if (other.hash == hash &&
                other.inst == inst)
                return true;
            return false;
        }
    }; // Value

    class LVNItemInfo
    {
    public:
        Value value;
        G4_INST* inst = nullptr;
        G4_Operand* opnd = nullptr;
        // if isImm is true then value.hash is immediate value
        bool isImm = false;
        unsigned int lb = 0;
        unsigned int rb = 0;
        bool isScalar = false;
        bool constHStride = false;
        unsigned int hstride = 0;
        // store all other LVNItemInfo* that refer to opnd.
        // this helps invalidate values accurately.
        std::vector<LVNItemInfo*> uses;
        // active field below determines whether value pointed to
        // by this instance is live. When a redef is seen for dst of an
        // LVN candidate we set active to false so the value is no longer
        // available for propagation. This is required because for an
        // instruction such as mov with dst and 1 src, we insert instance
        // of this class in 2 buckets - dst dcl id, src0 dcl id. Doing so
        // helps to easily invalidate values due to redefs.
        bool active = false;
    }; // LVNItemInfo
} // vISA::

// LvnTable uses dcl id or immediate value as key. This key is mapped to
// all operands with dcl id that have appeared so far in current BB. Or
// in case of immediates the key maps to respective operands. Having
// a map allows faster lookups and lesser number of comparisons than a
// running list of all instructions seen so far.
typedef std::unordered_map<int64_t, std::list<vISA::LVNItemInfo*>> LvnTable;
typedef struct UseInfo
{
    vISA::G4_INST* first;
    Gen4_Operand_Number second;
} UseInfo;
typedef std::list<UseInfo> UseList;
typedef std::list<vISA::G4_INST*> DefList;

typedef struct DefUseInfo
{
    vISA::G4_INST* first;
    UseList second;
} DefUseInfo;
typedef std::multimap<unsigned int, DefUseInfo> DefUseTable;

typedef struct ActiveDef
{
    vISA::G4_Declare* first;
    vISA::G4_DstRegRegion* second;
} ActiveDef;
typedef std::multimap<unsigned int, ActiveDef> ActiveDefMMap;

class GlobalDataAddrCleanup
{
public:
    std::unordered_map<vISA::G4_Declare*, unsigned int> addrVarDefCount;
    std::unordered_map<vISA::G4_BB*, unsigned int> addrVarDefCountPerBB;
};

namespace vISA
{
    class LVN
    {
    private:
        std::map<G4_INST*, UseList> defUse;
        std::map<G4_Operand*, DefList> useDef;
        std::unordered_map<G4_Declare*, std::list<LVNItemInfo*>> dclValueTable;
        G4_BB* bb;
        FlowGraph& fg;
        LvnTable lvnTable;
        ActiveDefMMap activeDefs;
        vISA::Mem_Manager& mem;
        IR_Builder& builder;
        unsigned int numInstsRemoved;
        bool duTablePopulated;
        PointsToAnalysis& p2a;
        std::vector<LVNItemInfo*> toDtor;
        std::vector<std::pair<G4_Declare*, LVNItemInfo*>> perInstValueCache;

        static const int MaxLVNDistance = 250;

        void populateDuTable(INST_LIST_ITER inst_it);
        void removeAddrTaken(G4_AddrExp* opnd);
        void addUse(G4_DstRegRegion* dst, G4_INST* use, unsigned int srcIndex);
        void addValueToTable(G4_INST* inst, Value& oldValue);
        LVNItemInfo* isValueInTable(Value& value, bool negate);
        bool isSameValue(Value& val1, Value& val2, bool negImmVal);
        bool computeValue(G4_INST* inst, bool negate, bool& canNegate, bool& isGlobal, int64_t& tmpPosImm, bool posValValid, Value& valueStr);
        bool addValue(G4_INST* inst);
        void getValue(G4_DstRegRegion* dst, G4_INST* inst, Value& value);
        void getValue(G4_SrcRegRegion* src, G4_INST* inst, Value& value);
        void getValue(int64_t imm, G4_Operand* opnd, Value& value);
        void getValue(G4_INST* inst, Value& value);
        const char* getModifierStr(G4_SrcModifier srcMod);
        int64_t getNegativeRepresentation(int64_t imm, G4_Type type);
        bool sameGRFRef(G4_Declare* dcl1, G4_Declare* dcl2);
        void removeVirtualVarRedefs(G4_DstRegRegion* dst);
        void removePhysicalVarRedefs(G4_DstRegRegion* dst);
        void removeRedefs(G4_INST* inst);
        void replaceAllUses(G4_INST* defInst, bool negate, UseList& uses, G4_INST* lvnInst, bool keepRegion);
        void transferAlign(G4_Declare* toDcl, G4_Declare* fromDcl);
        bool canReplaceUses(INST_LIST_ITER inst_it, UseList& uses, G4_INST* lvnInst, bool negMatch, bool noPartialUse);
        bool getAllUses(G4_INST* def, UseList& uses);
        bool getDstData(int64_t srcImm, G4_Type srcType, int64_t& dstImm, G4_Type dstType, bool& canNegate);
        bool valuesMatch(Value& val1, Value& val2, bool checkNegImm);
        void removeAliases(G4_INST* inst);
        bool checkIfInPointsTo(const G4_RegVar* addr, const G4_RegVar* var) const;
        template<class T, class K>
        bool opndsMatch(T*, K*);
        LVNItemInfo* getOpndValue(G4_Operand* opnd, bool create = true);
        void invalidateOldDstValue(G4_INST*);
        LVNItemInfo* createLVNItemInfo()
        {
            auto instance = new (mem.alloc(sizeof(LVNItemInfo))) LVNItemInfo;
            toDtor.push_back(instance);
            return instance;
        }

    public:
        LVN(FlowGraph& flowGraph, G4_BB* curBB, vISA::Mem_Manager& mmgr, IR_Builder& irBuilder, PointsToAnalysis& p) :
            fg(flowGraph), mem(mmgr), builder(irBuilder), p2a(p)
        {
            bb = curBB;
            numInstsRemoved = 0;
            duTablePopulated = false;
        }

        ~LVN();

        void doLVN();
        unsigned int getNumInstsRemoved() { return numInstsRemoved; }

        static unsigned int removeRedundantSamplerMovs(G4_Kernel&, G4_BB*);
        static unsigned int removeRedundantAddrAdd(G4_Kernel& kernel, G4_BB* bb,
            GlobalDataAddrCleanup& addrCleanup);
    };

    class CleanupAddrAdd {
    private:
        const unsigned int origInstCount;
        IR_Builder& builder;
        FlowGraph& fg;
        G4_BB* bb = nullptr;
        // store currently active defs
        std::list<G4_INST*> activeAddrDefs;
        // store map of old->new dcl
        std::unordered_map<G4_Declare*, G4_Declare*> replacementMap;
        // store count of addr var defs for BB
        std::unordered_map<G4_Declare*, unsigned int>& addrVarDefCount;
        // store information to invalidate based on WAR
        std::unordered_map<G4_Declare*, std::vector<G4_INST*>> war;

        G4_INST* getReplCand(G4_INST* other);
        void invalidate(std::vector<G4_INST*>& insts);
        void removeRedAddrAdd();
    public:
        CleanupAddrAdd(G4_Kernel& k, G4_BB* block,
            GlobalDataAddrCleanup& globalData) :
            origInstCount(block->size()), builder(*k.fg.builder), fg(k.fg),
            addrVarDefCount(globalData.addrVarDefCount)
        {
            bb = block;
        }

        void run();
        void dump(std::ostream& OS) const;

        static void getAddrVarDataGlobal(FlowGraph& fg, GlobalDataAddrCleanup& addrCleanup)
        {
            auto& addrVarDefCount = addrCleanup.addrVarDefCount;
            auto& addrVarDefCountPerBB = addrCleanup.addrVarDefCountPerBB;
            for (auto bblock : fg)
            {
                for (auto inst : *bblock)
                {
                    auto dst = inst->getDst();
                    if (dst)
                    {
                        if (auto topdcl = dst->getTopDcl())
                        {
                            if (topdcl->getRegFile() == G4_ADDRESS &&
                                dst->isDirect())
                            {
                                addrVarDefCount[topdcl] += 1;
                                addrVarDefCountPerBB[bblock] += 1;
                            }
                        }
                    }
                }
            }
        }
    };
}
#endif
