/*========================== begin_copyright_notice ============================

Copyright (C) 2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#include <algorithm>
#include <string>
#include <unordered_map>
#include <iostream>
#include <fstream>

#include <iomanip>
#include <common/igc_regkeys.hpp>
#include <Metrics/IGCMetricImpl.h>

#include <Probe/Assertion.h>
#include <Compiler/CISACodeGen/ShaderCodeGen.hpp>
#include <DebugInfo/VISAModule.hpp>
#include <Compiler/DebugInfo/ScalarVISAModule.h>
#include <visaBuilder_interface.h>
#include <visa/Common_ISA.h>

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Path.h>
#include <llvm/IR/DebugInfo.h>
#include "common/LLVMWarningsPop.hpp"

//#define DEBUG_METRIC

namespace IGCMetrics
{
    IGCMetricImpl::IGCMetricImpl()
    {
        this->isEnabled = false;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        this->countInstInFunc = 0;
#endif
    }
    IGCMetricImpl::~IGCMetricImpl()
    {
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        this->map_EmuCalls.clear();
        this->map_Func.clear();
        this->map_Loops.clear();
#endif
    }
    bool IGCMetricImpl::Enable()
    {
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        return isEnabled;
#else
        return false;
#endif
    }

    void IGCMetricImpl::Init(ShaderHash* Hash, bool isEnabled)
    {
        this->isEnabled = isEnabled;
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        std::stringstream ss;

        ss << std::hex
            << std::setfill('0')
            << std::setw(sizeof(Hash->asmHash) * CHAR_BIT / 4)
            << Hash->asmHash;

        oclProgram.set_hash(ss.str());
#endif
    }

    void IGCMetricImpl::OutputMetrics()
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED

        if (IGC_GET_FLAG_VALUE(MetricsDumpEnable) > 0)
        {
            // Out file with ext OPTRPT - OPTimization RePoT
            std::string fileName = oclProgram.hash() + ".optrpt";

            std::ofstream metric_data;
            metric_data.open(fileName);

            if (metric_data.is_open())
            {
                if (IGC_GET_FLAG_VALUE(MetricsDumpEnable) == 1)
                {
                    // Binary format of protobuf
                    oclProgram.SerializePartialToOstream(&metric_data);
                }
                else if (IGC_GET_FLAG_VALUE(MetricsDumpEnable) == 2)
                {
                    // Text readable in JSON format
                    google::protobuf::util::JsonPrintOptions jsonConfig;

                    jsonConfig.add_whitespace = true;
                    jsonConfig.preserve_proto_field_names = true;
                    jsonConfig.always_print_primitive_fields = true;

                    std::string json;
                    google::protobuf::util::MessageToJsonString(oclProgram, &json, jsonConfig);
                    metric_data << json;
                }

                metric_data.close();
            }
        }
#endif
    }

    void IGCMetricImpl::StatBeginEmuFunc(llvm::Instruction* instruction)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        countInstInFunc = CountInstInFunc(instruction->getParent()->getParent());
#endif
    }

    bool isDPType(llvm::Instruction* instruction)
    {
        llvm::Type* type = instruction->getType()->getScalarType();
        if (type->isDoubleTy())
        {
            return true;
        }

        for (unsigned int i = 0; i < instruction->getNumOperands(); ++i)
        {
            type = instruction->getOperand(i)->getType()->getScalarType();
            if (type->isDoubleTy())
            {
                return true;
            }
        }

        return false;
    }

    void IGCMetricImpl::StatEndEmuFunc(llvm::Instruction* emulatedInstruction)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        llvm::DILocation* debLoc = (llvm::DILocation*)emulatedInstruction->getDebugLoc();

        auto func_m = GetFuncMetric(emulatedInstruction);
        if (func_m == nullptr)
        {
            return;
        }

        IGC_METRICS::InstrStats* stats = func_m->mutable_instruction_stats();
        IGC_METRICS::FuncEmuCalls* emuCall_m = nullptr;

        // Count how many instructions we added
        int extraInstrAdded = CountInstInFunc(emulatedInstruction->getParent()->getParent()) -
            countInstInFunc;
        // reset counter
        countInstInFunc = 0;

        if (map_EmuCalls.find(debLoc) != map_EmuCalls.end())
        {
            // For case when receive extra instruction to already recoreded emu-function
            emuCall_m = map_EmuCalls[debLoc];
        }
        else
        {
            // For case if we discover new emulated function
            emuCall_m = func_m->add_emufunctioncalls();
            map_EmuCalls.insert({ debLoc, emuCall_m });

            auto emuCall_m_loc = emuCall_m->add_funccallloc();
            FillCodeRef(emuCall_m_loc, debLoc);
            stats->set_countemulatedinst(stats->countemulatedinst() + 1);

            if (IGC_IS_FLAG_ENABLED(ForceDPEmulation) && isDPType(emulatedInstruction))
            {
                emuCall_m->set_type(IGC_METRICS::FuncEmuCalls_Reason4FuncEmu_FP_MODEL_MODE);
            }
            else
            {
                emuCall_m->set_type(IGC_METRICS::FuncEmuCalls_Reason4FuncEmu_NO_HW_SUPPORT);
            }
        }
        // Count amount of instructions created to emulate not supported instruction
        emuCall_m->set_count(emuCall_m->count() + extraInstrAdded);
#endif
    }

    void IGCMetricImpl::StatIncCoalesced(llvm::Instruction* coalescedAccess)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        auto func_m = GetFuncMetric(coalescedAccess);
        if (func_m == nullptr)
        {
            return;
        }

        IGC_METRICS::InstrStats* stats = func_m->mutable_instruction_stats();
        stats->set_countcoalescedaccess(stats->countcoalescedaccess() + 1);
#endif
    }

    void IGCMetricImpl::CollectRegStats(KERNEL_INFO* kernelInfo)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        if (kernelInfo == nullptr)
        {
            return;
        }
#endif
    }

    void IGCMetricImpl::CollectFunctions(llvm::Module* pModule)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED

        this->pModule = (IGCLLVM::Module*)pModule;
        for (auto func_i = pModule->begin(); func_i != pModule->end(); ++func_i)
        {
            llvm::Function& func = *func_i;
            llvm::DISubprogram* func_dbinfo = func.getSubprogram();

            if (func_dbinfo != nullptr)
            {
                IGC_METRICS::Function* func_m = oclProgram.add_functions();

                func_m->set_name(func.getName().str());

                switch (func.getCallingConv())
                {
                case llvm::CallingConv::SPIR_FUNC:
                    func_m->set_type(IGC_METRICS::FunctionType::FUNCTION);
                    break;
                case llvm::CallingConv::SPIR_KERNEL:
                    func_m->set_type(IGC_METRICS::FunctionType::KERNEL);
                    break;
                case llvm::CallingConv::C:
                    func_m->set_type(IGC_METRICS::FunctionType::FUNCTION);
                    break;
                default:
                    IGC_ASSERT_MESSAGE(false, "Unknow Function type");
                    break;
                }
                map_Func.insert({ func_dbinfo , func_m });
                IGC_METRICS::CodeRef* func_m_loc = func_m->mutable_funcloc();
                FillCodeRef(func_m_loc, func_dbinfo);

                GetFunctionData(func_m, func);
            }
        }
#endif
    }

    void IGCMetricImpl::CollectLoops(llvm::Loop* loop)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        for (auto subLoop : loop->getSubLoops())
        {
            CollectLoop(subLoop);
        }
#endif
    }

    void IGCMetricImpl::CollectLoops(llvm::LoopInfo* loopInfo)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        for (auto loop_i = loopInfo->begin(); loop_i != loopInfo->end(); ++loop_i)
        {
            llvm::Loop* loop = *loop_i;
            CollectLoop(loop);
            CollectLoops(loop);
        }
#endif
    }

    void IGCMetricImpl::CollectLoopCyclomaticComplexity(
        llvm::Function* pFunc,
        int LoopCyclomaticComplexity,
        int LoopCyclomaticComplexity_Max)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        IGC_METRICS::Function* func_metric = GetFuncMetric(pFunc);

        if (func_metric)
        {
            auto costmodel = func_metric->mutable_costmodel_stats();
            auto simd16cost = costmodel->mutable_simd16();
            simd16cost->set_loopcyclomaticcomplexity(LoopCyclomaticComplexity);
            simd16cost->set_loopcyclomaticcomplexity_max(LoopCyclomaticComplexity_Max);
            simd16cost->set_loopcyclomaticcomplexity_status(
                LoopCyclomaticComplexity < LoopCyclomaticComplexity_Max ?
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_OK :
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_BAD);
        }
#endif
    }

    void IGCMetricImpl::CollectNestedLoopsWithMultipleExits(
        llvm::Function* pFunc,
        float NestedLoopsWithMultipleExitsRatio,
        float NestedLoopsWithMultipleExitsRatio_Max)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        IGC_METRICS::Function* func_metric = GetFuncMetric(pFunc);

        if (func_metric)
        {
            auto costmodel = func_metric->mutable_costmodel_stats();
            auto simd16cost = costmodel->mutable_simd16();
            simd16cost->set_nestedloopswithmultipleexitsratio(NestedLoopsWithMultipleExitsRatio);
            simd16cost->set_nestedloopswithmultipleexitsratio_max(NestedLoopsWithMultipleExitsRatio_Max);
            simd16cost->set_nestedloopswithmultipleexitsratio_status(
                NestedLoopsWithMultipleExitsRatio < NestedLoopsWithMultipleExitsRatio_Max ?
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_OK :
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_BAD);
        }
#endif
    }

    void IGCMetricImpl::CollectLongStridedLdStInLoop(
        llvm::Function* pFunc,
        llvm::Loop* pProblematicLoop,
        int LongStridedLdStInLoop_LdCnt,
        int LongStridedLdStInLoop_StCnt,
        int LongStridedLdStInLoop_MaxCntLdOrSt)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        IGC_METRICS::Function* func_metric = GetFuncMetric(pFunc);

        if (func_metric)
        {
            auto costmodel = func_metric->mutable_costmodel_stats();
            auto simd16cost = costmodel->mutable_simd16();

            if (pProblematicLoop == nullptr)
            {
                simd16cost->set_longstridedldstinloop_status(
                    IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_OK);
            }
            else
            {
                simd16cost->set_longstridedldstinloop_status(
                    IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_BAD);
                simd16cost->set_longstridedldstinloop_ldcnt(LongStridedLdStInLoop_LdCnt);
                simd16cost->set_longstridedldstinloop_stcnt(LongStridedLdStInLoop_StCnt);
                simd16cost->set_longstridedldstinloop_maxcntldorst(LongStridedLdStInLoop_MaxCntLdOrSt);

                FillCodeRef(simd16cost->mutable_longstridedldstinloop_problematicloop(),
                    pProblematicLoop->getStartLoc());
            }
        }
#endif
    }

    void IGCMetricImpl::CollectIsGeminiLakeWithDoubles(
        llvm::Function* pFunc,
        bool IsGeminiLakeWithDoubles)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        IGC_METRICS::Function* func_metric = GetFuncMetric(pFunc);

        if (func_metric)
        {
            auto costmodel = func_metric->mutable_costmodel_stats();
            auto simd16cost = costmodel->mutable_simd16();

            simd16cost->set_isgeminilakewithdoubles_status(IsGeminiLakeWithDoubles ?
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_OK :
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_BAD);
        }
#endif
    }

    void IGCMetricImpl::FinalizeStats()
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        UpdateLoopsInfo();
        UpdateModelCost();
        UpdateFunctionArgumentsList();
#endif
    }

    void IGCMetricImpl::CollectDataFromDebugInfo(IGC::DebugInfoData* pDebugInfo, IGC::DbgDecoder* pDebugDecoder)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED

        oclProgram.set_device((IGC_METRICS::DeviceType)
            pDebugInfo->m_pShader->m_Platform->getPlatformInfo().eProductFamily);

        llvm::DenseMap<llvm::Function*, IGC::VISAModule*>* pListFuncData =
            &pDebugInfo->m_VISAModules;
        llvm::DenseMap< llvm::DIVariable*, IGC_METRICS::VarInfo*> var_db2metric;

        for (auto pListFuncData_it = pListFuncData->begin();
            pListFuncData_it != pListFuncData->end(); ++pListFuncData_it)
        {
            llvm::Function* pFunc = pListFuncData_it->first;
            IGC::VISAModule* vISAData = pListFuncData_it->second;

            //IGC::ScalarVisaModule* scalarVM = llvm::dyn_cast<IGC::ScalarVisaModule>(vISAData);

#ifdef DEBUG_METRIC
            std::printf("\nList of symbols:\n");

            for (auto it_dbgInfo = pDebugInfo->m_FunctionSymbols[pFunc].begin();
                it_dbgInfo != pDebugInfo->m_FunctionSymbols[pFunc].end(); ++it_dbgInfo)
            {
                std::printf("pointer{%p} key{%s} val{%s}\n",
                    it_dbgInfo->first,
                    it_dbgInfo->first->getName().str().c_str(), it_dbgInfo->second->getName().getCString());
                it_dbgInfo->first->dump();
            }
#endif

            const llvm::Value* pVal = nullptr;

            // Iterate over all instruction ported to vISA
            for (auto instr = vISAData->begin(); instr != vISAData->end(); ++instr)
            {
                if (const llvm::DbgDeclareInst* pDbgAddrInst =
                    llvm::dyn_cast<llvm::DbgDeclareInst>(*instr))
                {
                    // Get : call void @llvm.dbg.value
                    pVal = pDbgAddrInst->getAddress();
                }
                else if (const llvm::DbgValueInst* pDbgValInst =
                    llvm::dyn_cast<llvm::DbgValueInst>(*instr))
                {
                    // Get : call void @llvm.dbg.value

                    // Avoid undef values in metadata
                    {
                        llvm::MetadataAsValue* mdAv = llvm::dyn_cast<llvm::MetadataAsValue>(pDbgValInst->getArgOperand(0));
                        if (mdAv != nullptr)
                        {
                            llvm::ValueAsMetadata* vAsMD = llvm::dyn_cast<llvm::ValueAsMetadata>(mdAv->getMetadata());
                            if (vAsMD != nullptr &&
                                llvm::isa<llvm::UndefValue>(vAsMD->getValue()))
                            {
                                continue;
                            }
                        }
                    }

                    pVal = pDbgValInst->getValue();
                }
                else
                {
                    continue;
                }

                auto varLoc = vISAData->GetVariableLocation(*instr);

                IGC_METRICS::VarInfo* varInfo_m = GetVarMetric((llvm::Value*)pVal);

                if (varInfo_m == nullptr)
                {
                    continue;
                }

                // Get CVariable data for this user variable
                auto cvar = pDebugInfo->getMapping(*pFunc, pVal);

#ifdef DEBUG_METRIC
                int users_count = (int)std::distance(pVal->user_begin(), pVal->user_end());
                pVal->dump();
                std::printf("\ninstr (varname:%s, pointer:%p, usage count:%d) :\n", varInfo_m->name().c_str(), pVal, users_count);
                (*instr)->dump();
#endif

                if (!varLoc.IsRegister() &&
                    !varLoc.IsImmediate() &&
                    !varLoc.IsSLM())
                {
                    continue;
                }
                // As for now support only registers, immediates and slm memory to report

                if (cvar == nullptr &&
                    pDebugInfo->m_pShader->GetSymbolMapping().find((llvm::Value*)pVal)
                    != pDebugInfo->m_pShader->GetSymbolMapping().end())
                {
                    cvar = pDebugInfo->m_pShader->GetSymbolMapping()[(llvm::Value*)pVal];
                }

                if (cvar == nullptr &&
                    pDebugInfo->m_pShader->GetGlobalMapping().find((llvm::Value*)pVal)
                    != pDebugInfo->m_pShader->GetGlobalMapping().end())
                {
                    cvar = pDebugInfo->m_pShader->GetGlobalMapping()[(llvm::Value*)pVal];
                }

                if (cvar == nullptr)
                {
                    auto cvar_const = llvm::dyn_cast<llvm::Constant>(pVal);
                    if (cvar_const != nullptr &&
                        pDebugInfo->m_pShader->GetConstantMapping().find((llvm::Constant*)cvar_const)
                        != pDebugInfo->m_pShader->GetConstantMapping().end())
                    {
                        cvar = pDebugInfo->m_pShader->GetConstantMapping()[(llvm::Constant*)cvar_const];
                    }
                }

                if (cvar == nullptr)
                {
                    // If not found check in whole shader data
                    cvar = pDebugInfo->m_pShader->GetSymbol((llvm::Value*)pVal, false);
                }

                    varInfo_m->set_size(cvar->GetSize());
                    varInfo_m->set_type((IGC_METRICS::VarInfo_VarType)cvar->GetType());

                auto fillRegister = [&](unsigned int reg)
                {
                    const auto* varInfo = vISAData->getVarInfo(*pDebugDecoder, reg);
                    auto varInfo_reg_m = varInfo_m->add_reg();

                    varInfo_reg_m->set_addrmodel(varLoc.IsInGlobalAddrSpace() ?
                        IGC_METRICS::VarInfo_AddressModel::VarInfo_AddressModel_GLOBAL :
                        IGC_METRICS::VarInfo_AddressModel::VarInfo_AddressModel_LOCAL);

                    //varInfo_m->set_memoryaccess((IGC_METRICS::VarInfo_MemAccess)varInfo->memoryAccess);

                    if (varInfo != nullptr)
                    {
                        // check if any?
                        varInfo_reg_m->set_isspill(varInfo->lrs[0].isSpill());
                        varInfo_reg_m->set_liverangestart(varInfo->lrs[0].start);
                        varInfo_reg_m->set_liverangeend(varInfo->lrs[0].end);
                    }
                    varInfo_reg_m->set_isuniform(cvar->IsUniform());
                    varInfo_reg_m->set_isconst(cvar->IsImmediate());
                };

                fillRegister(varLoc.GetRegister());
                // Special case when we have simd32 splitted into two simd16
                if (varLoc.HasLocationSecondReg())
                {
                    fillRegister(varLoc.GetSecondReg());
                }
            }
        }
#endif
    }


    void IGCMetricImpl::CollectInstructionCnt(
        llvm::Function* pFunc,
        int InstCnt,
        int InstCntMax)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        IGC_METRICS::Function* func_metric = GetFuncMetric(pFunc);

        if (func_metric)
        {
            auto costmodel = func_metric->mutable_costmodel_stats();
            auto simd32cost = costmodel->mutable_simd32();

            simd32cost->set_instructioncount(InstCnt);
            simd32cost->set_instructioncount_max(InstCntMax);
            simd32cost->set_instructioncount_status(InstCnt < InstCntMax ?
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_OK :
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_BAD);
        }
#endif
    }

    void IGCMetricImpl::CollectThreadGroupSize(
        llvm::Function* pFunc,
        int ThreadGroupSize,
        int ThreadGroupSizeMax)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        IGC_METRICS::Function* func_metric = GetFuncMetric(pFunc);

        if (func_metric)
        {
            auto costmodel = func_metric->mutable_costmodel_stats();
            auto simd32cost = costmodel->mutable_simd32();

            simd32cost->set_threadgroupsize(ThreadGroupSize);
            simd32cost->set_threadgroupsize_max(ThreadGroupSizeMax);
            simd32cost->set_threadgroupsize_status(ThreadGroupSize < ThreadGroupSizeMax ?
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_OK :
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_BAD);
        }
#endif
    }

    void IGCMetricImpl::CollectThreadGroupSizeHint(
        llvm::Function* pFunc,
        int ThreadGroupSizeHint,
        int ThreadGroupSizeHintMax)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        IGC_METRICS::Function* func_metric = GetFuncMetric(pFunc);

        if (func_metric)
        {
            auto costmodel = func_metric->mutable_costmodel_stats();
            auto simd32cost = costmodel->mutable_simd32();

            simd32cost->set_threadgroupsizehint(ThreadGroupSizeHint);
            simd32cost->set_threadgroupsizehint_max(ThreadGroupSizeHintMax);
            simd32cost->set_threadgroupsizehint_status(ThreadGroupSizeHint < ThreadGroupSizeHintMax ?
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_OK :
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_BAD);
        }
#endif
    }

    void IGCMetricImpl::CollectIsSubGroupFuncIn(
        llvm::Function* pFunc,
        bool flag)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        IGC_METRICS::Function* func_metric = GetFuncMetric(pFunc);

        if (func_metric)
        {
            auto costmodel = func_metric->mutable_costmodel_stats();
            auto simd32cost = costmodel->mutable_simd32();

            simd32cost->set_subgroupfunctionarepresent_status(!flag ?
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_OK :
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_BAD);
        }
#endif
    }

    void IGCMetricImpl::CollectGen9Gen10WithIEEESqrtDivFunc(
        llvm::Function* pFunc,
        bool flag)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        IGC_METRICS::Function* func_metric = GetFuncMetric(pFunc);

        if (func_metric)
        {
            auto costmodel = func_metric->mutable_costmodel_stats();
            auto simd32cost = costmodel->mutable_simd32();

            simd32cost->set_gen9orgen10withieeesqrtordivfunc_status(!flag ?
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_OK :
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_BAD);
        }
#endif
    }

    enum {
        LOOPCOUNT_LIKELY_SMALL,
        LOOPCOUNT_LIKELY_LARGE,
        LOOPCOUNT_UNKNOWN
    };

    void IGCMetricImpl::CollectNonUniformLoop(
        llvm::Function* pFunc,
        short LoopCount,
        llvm::Loop* problematicLoop)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        IGC_METRICS::Function* func_metric = GetFuncMetric(pFunc);

        if (func_metric)
        {
            auto costmodel = func_metric->mutable_costmodel_stats();
            auto simd32cost = costmodel->mutable_simd32();

            if (problematicLoop == nullptr)
            {
                simd32cost->set_nonuniformloop_status(
                    IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_OK);
                simd32cost->set_nonuniformloop_count(
                    IGC_METRICS::CostModelStats_CostSIMD32_LoopCount::CostModelStats_CostSIMD32_LoopCount_LIKELY_SMALL);
            }
            else
            {
                simd32cost->set_nonuniformloop_status(
                    IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_BAD);
                simd32cost->set_nonuniformloop_count(
                    (IGC_METRICS::CostModelStats_CostSIMD32_LoopCount)LoopCount);
                auto codeRefloop = simd32cost->mutable_nonuniformloop_problematicloop();
                FillCodeRef(codeRefloop, problematicLoop->getStartLoc());
            }
        }
#endif
    }

    void IGCMetricImpl::CollectMem2Reg(llvm::AllocaInst* pAllocaInst, IGC::StatusPrivArr2Reg status)
    {
        if (!Enable()) return;
#ifdef IGC_METRICS__PROTOBUF_ATTACHED
        IGC_METRICS::Function* func_metric = GetFuncMetric(pAllocaInst);
        if (func_metric)
        {
            IGC_METRICS::VarInfo* var_m = GetVarMetric(pAllocaInst);

            if (var_m)
            {
                var_m->set_status_privarr2reg(
                    (IGC_METRICS::VarInfo_PrivArr2Reg)status);
            }
        }
#endif
    }

#ifdef IGC_METRICS__PROTOBUF_ATTACHED

    void IGCMetricImpl::UpdateFunctionArgumentsList()
    {
        for (auto func_i = pModule->begin();
            func_i != pModule->end(); ++func_i)
        {
            llvm::Function* func = &*func_i;

            auto func_m = GetFuncMetric(func);

            if (func_m)
            {
                for (auto arg_i = func->arg_begin();
                    arg_i != func->arg_end(); ++arg_i)
                {
                    llvm::Argument* arg = &*arg_i;
                    bool foundInMetric = false;

                    // Check if we are looking on the explicit argument
                    // which is already added in the metrics for the function
                    if (arg->hasName())
                    {
                        for (int i = 0; i < func_m->arguments_size(); ++i)
                        {
                            if (func_m->arguments(i).name() == arg->getName().str())
                            {
                                foundInMetric = true;
                                break;
                            }
                        }
                    }

                    // Not found - add it as implict argument
                    if (!foundInMetric)
                    {
                        auto func_arg_m = func_m->add_arguments();
                        if (arg->hasName())
                        {
                            func_arg_m->set_name(arg->getName().str());
                        }
                        func_arg_m->set_compilesize(arg->getType()->getPrimitiveSizeInBits());
                        func_arg_m->set_type(IGC_METRICS::KernelArg_ArgumentType::KernelArg_ArgumentType_IMPLICIT);
                    }
                }
            }
        }
    }

    void IGCMetricImpl::UpdateModelCost()
    {
        // Function which checks the overall model cost of kernel status for SIMD16 and SIMD32

        auto isOkStatus = [](IGC_METRICS::CostModelStats_CostStatus Status)
        {
            return Status !=
                IGC_METRICS::CostModelStats_CostStatus::CostModelStats_CostStatus_BAD;
        };

        for (auto func_m_i = map_Func.begin(); func_m_i != map_Func.end(); ++func_m_i)
        {
            auto func_m = func_m_i->second;

            if (func_m->has_costmodel_stats())
            {
                auto costmodel = func_m->mutable_costmodel_stats();

                if (costmodel->has_simd16())
                {
                    auto simd16 = costmodel->mutable_simd16();

                    simd16->set_overallstatus(
                        isOkStatus(simd16->loopcyclomaticcomplexity_status()) &&
                        isOkStatus(simd16->nestedloopswithmultipleexitsratio_status()) &&
                        isOkStatus(simd16->longstridedldstinloop_status()) &&
                        isOkStatus(simd16->isgeminilakewithdoubles_status()));
                }

                if (costmodel->has_simd32())
                {
                    auto simd32 = costmodel->mutable_simd32();

                    simd32->set_overallstatus(
                        isOkStatus(simd32->instructioncount_status()) &&
                        isOkStatus(simd32->threadgroupsize_status()) &&
                        isOkStatus(simd32->threadgroupsizehint_status()) &&
                        isOkStatus(simd32->subgroupfunctionarepresent_status()) &&
                        isOkStatus(simd32->gen9orgen10withieeesqrtordivfunc_status()) &&
                        isOkStatus(simd32->nonuniformloop_status()));
                }
            }
        }
    }

    void IGCMetricImpl::CollectLoop(llvm::Loop* loop)
    {
        if (loop->getStartLoc() && loop->getStartLoc()->getScope())
        {
            if (map_Loops.find(loop->getStartLoc()->getScope()) == map_Loops.end())
            {
                auto func_m = GetFuncMetric(loop);
                if (func_m == nullptr)
                {
                    return;
                }

                auto cfg_stats = func_m->mutable_cfg_stats();
                auto loop_m = cfg_stats->add_loops_stats();
                auto loopLoc = loop_m->mutable_looploc();

                FillCodeRef(loopLoc, loop->getStartLoc());
                loop_m->set_nestinglevel(loop->getLoopDepth());

                map_Loops.insert({ loop->getStartLoc()->getScope(), loop_m });
            }
        }
    }

    void IGCMetricImpl::UpdateLoopsInfo()
    {
        //TODO
    }

    class CollectDataMetrics : public llvm::InstVisitor<CollectDataMetrics>
    {
        IGCMetricImpl* metric;

    public:

        CollectDataMetrics(IGCMetricImpl* metric)
        {
            this->metric = metric;
        }

        void visitDbgVariableIntrinsic(llvm::DbgVariableIntrinsic& dbValInst)
        {
            metric->AddVarMetric(&dbValInst);
        }

        void visitCallInst(llvm::CallInst& callInst)
        {
            auto calledFuncName = callInst.getCalledFunction()->getName();
            if (calledFuncName.startswith("llvm.dbg") ||
                calledFuncName.startswith("llvm.genx.GenISA.CatchAllDebugLine"))
            {
                // Ignore debugInfo calls
                return;
            }


            auto func_m = metric->GetFuncMetric(&callInst);
            auto funcCallType = IGC_METRICS::FuncCalls_FuncCallsType::FuncCalls_FuncCallsType_INLINE;

            if (calledFuncName.startswith("__builtin_IB"))
            {
                funcCallType = IGC_METRICS::FuncCalls_FuncCallsType::FuncCalls_FuncCallsType_LIBRARY;
            }
            else if (calledFuncName.startswith("llvm."))
            {
                funcCallType = IGC_METRICS::FuncCalls_FuncCallsType::FuncCalls_FuncCallsType_LIBRARY;
            }
            else if (calledFuncName.startswith("__builtin_spirv"))
            {
                funcCallType = IGC_METRICS::FuncCalls_FuncCallsType::FuncCalls_FuncCallsType_LIBRARY;
            }

            // Get data about this function call
            IGC_METRICS::FuncCalls* callFunc_m = nullptr;

            for (int i = 0; i < func_m->functioncalls_size(); ++i)
            {
                if (calledFuncName.equals(
                    func_m->functioncalls(i).name()))
                {
                    // For case if we have already record created
                    callFunc_m = (IGC_METRICS::FuncCalls*) & func_m->functioncalls(i);
                    callFunc_m->set_count(callFunc_m->count() + 1);
                    break;
                }
            }
            if (callFunc_m == nullptr)
            {
                // For new case
                callFunc_m = func_m->add_functioncalls();
                callFunc_m->set_name(calledFuncName.str());
                callFunc_m->set_count(1);
                callFunc_m->set_type(funcCallType);
            }

            auto instr_call_dbinfo = callInst.getDebugLoc();
            auto callFunc_m_loc = callFunc_m->add_funccallloc();
            metric->FillCodeRef(callFunc_m_loc, instr_call_dbinfo);
        }
    };

    void IGCMetricImpl::GetFunctionData(IGC_METRICS::Function* func_m, llvm::Function& func)
    {
        CollectDataMetrics metricPass(this);

        metricPass.visit(func);
    }

    int IGCMetricImpl::CountInstInFunc(llvm::Function* pFunc)
    {
        unsigned int instCount = 0;
        for (auto bb = pFunc->begin(); bb != pFunc->end(); ++bb)
        {
            instCount += (unsigned int)std::distance(bb->begin(), bb->end());
        }

        return instCount;
    }

    IGC_METRICS::VarInfo* IGCMetricImpl::AddVarMetric(llvm::DbgVariableIntrinsic* pInstr)
    {
        llvm::MDNode* pNode = nullptr;
        llvm::Value* value = nullptr;
        llvm::MetadataAsValue* MDValue = llvm::dyn_cast<llvm::MetadataAsValue>(pInstr->getArgOperand(0));
        llvm::MetadataAsValue* MDDILocalVariable = llvm::dyn_cast<llvm::MetadataAsValue>(pInstr->getArgOperand(1));
        IGC_METRICS::VarInfo* var_m = nullptr;

        if (MDValue != nullptr)
        {
            llvm::ValueAsMetadata* vAsMD = llvm::dyn_cast<llvm::ValueAsMetadata>(MDValue->getMetadata());
            pNode = pInstr->getVariable();
            if (vAsMD != nullptr &&
                vAsMD->getValue() != nullptr)
            {
                value = vAsMD->getValue();
            }
        }

        if (pNode && value)
        {
            // Map only once user variable in metrics
            if (map_Var.find(MDDILocalVariable) == map_Var.end())
            {
                // Extract debuginfo variable data to metrics
                llvm::DIVariable* diVar = llvm::cast<llvm::DIVariable>(pNode);

                std::string varName = diVar->getName().str();

                auto func_m = GetFuncMetric(pInstr);

                var_m = func_m->add_variables();
                var_m->set_name(varName);
                FillCodeRef(var_m->mutable_varloc(), diVar);

                // If variable is an argument of function/kernel
                // make a record of this information in metric too
                if (llvm::isa<llvm::Argument>(value))
                {
                    auto func_arg_m = func_m->add_arguments();
                    func_arg_m->set_name(varName);
                    func_arg_m->set_compilesize(value->getType()->getPrimitiveSizeInBits());
                    func_arg_m->set_type(IGC_METRICS::KernelArg_ArgumentType::KernelArg_ArgumentType_EXPLICIT);
                }

                // The user variables are identified by the MDAsVal,
                // because they are unique in whole module and aren't
                // recreated/changed during compilation of shader (it doesn't change pointer)
                map_Var[MDDILocalVariable] = var_m;
            }
            else
            {
                var_m = map_Var[MDDILocalVariable];
            }

            // Map in code any refrence to this variable (for metrics)
            // by adding callinstr llvm.igc.metric.trackValue in module for tracking
            llvm::AttributeList atrr;
            atrr.addAttribute(pModule->getContext(), 0, llvm::Attribute::AttrKind::OptimizeNone);
            atrr.addAttribute(pModule->getContext(), 1, llvm::Attribute::AttrKind::NoInline);
            atrr.addAttribute(pModule->getContext(), 2, llvm::Attribute::AttrKind::ReadNone);
            atrr.addAttribute(pModule->getContext(), 3, llvm::Attribute::AttrKind::NoAlias);

            auto funcType = llvm::FunctionType::get(
                llvm::Type::getVoidTy(pModule->getContext()),
                { llvm::Type::getMetadataTy(pModule->getContext()), llvm::Type::getMetadataTy(pModule->getContext()) }, false);

            auto funcVal = pModule->getOrInsertFunction(funcTrackValue, funcType, atrr);

            llvm::Function* func = llvm::cast<llvm::Function>(funcVal);

            llvm::CallInst::Create(func, { MDValue, MDDILocalVariable }, "", pInstr);

            return var_m;
        }
        // Cannot find associated user-variable with this instruction
        return nullptr;
    }

    IGC_METRICS::VarInfo* IGCMetricImpl::GetVarMetric(llvm::Value* pValue)
    {
        map_Var.begin();
        // iterate over all user variables which we found
        for (auto trackerVal_i = map_Var.begin();
            trackerVal_i != map_Var.end();
            ++trackerVal_i)
        {
            // The user variables are identified by the MDAsVal,
            // because they are unique in whole module and aren't
            // recreated/changed during compilation of shader (it doesn't change pointer)
            llvm::MetadataAsValue* tracker = (*trackerVal_i).first;

            for (auto user : tracker->users())
            {
                // Check all usage of this MDAsVal and look for the metrics call functions:
                // call void @llvm.igc.metric.trackValue(...)
                if (llvm::CallInst* callInst = dyn_cast<llvm::CallInst>(user))
                {
                    if (callInst->getCalledFunction()->getName().startswith(funcTrackValue))
                    {
                        llvm::Value* trackedValue = callInst->getArgOperand(0);

                        llvm::MetadataAsValue* MDValue = llvm::dyn_cast<llvm::MetadataAsValue>(trackedValue);
                        llvm::ValueAsMetadata* vAsMD = llvm::dyn_cast<llvm::ValueAsMetadata>(MDValue->getMetadata());

                        // Found tracker which looks at defined user variable
                        if (vAsMD && vAsMD->getValue() == pValue)
                        {
                            return map_Var[tracker];
                        }
                    }
                }
            }
        }
        // Cannot find associated user-variable with this instruction/value
        return nullptr;
    }

    IGC_METRICS::Function* IGCMetricImpl::GetFuncMetric(const llvm::Instruction* const pInstr)
    {
        return GetFuncMetric((llvm::Instruction*)pInstr);
    }

    IGC_METRICS::Function* IGCMetricImpl::GetFuncMetric(llvm::Instruction* pInstr)
    {
        auto func_m = GetFuncMetric(&pInstr->getDebugLoc());
        if (func_m != nullptr)
        {
            return func_m;
        }
        return GetFuncMetric(pInstr->getParent()->getParent());
    }

    IGC_METRICS::Function* IGCMetricImpl::GetFuncMetric(llvm::Loop* pLoop)
    {
        auto func_m = GetFuncMetric(&pLoop->getStartLoc());
        if (func_m != nullptr)
        {
            return func_m;
        }
        return GetFuncMetric(pLoop->getBlocks()[0]->getParent());
    }

    IGC_METRICS::Function* IGCMetricImpl::GetFuncMetric(llvm::Function* pFunc)
    {
        return GetFuncMetric(pFunc->getSubprogram());
    }

    IGC_METRICS::Function* IGCMetricImpl::GetFuncMetric(const llvm::DebugLoc* pLoc)
    {
        if (pLoc == nullptr || !pLoc->get())
        {
            return nullptr;
        }
        const MDNode* Scope = pLoc->getInlinedAtScope();
        if (auto* SP = llvm::getDISubprogram(Scope))
        {
            return GetFuncMetric(SP);
        }
        return nullptr;
    }

    IGC_METRICS::Function* IGCMetricImpl::GetFuncMetric(llvm::DISubprogram* pFunc)
    {
        if (map_Func.find(pFunc) == map_Func.end())
        {
            return nullptr;
        }
        else
        {
            return map_Func[pFunc];
        }
    }

    void IGCMetricImpl::FillCodeRef(IGC_METRICS::CodeRef* codeRef, llvm::DISubprogram* Loc)
    {
        if (Loc == nullptr || Loc->getDirectory().empty() || Loc->getFilename().empty())
        {
            return;
        }
        FillCodeRef(codeRef, GetFullPath(Loc->getDirectory().str(), Loc->getFilename().str()),
            Loc->getLine());
    }

    void IGCMetricImpl::FillCodeRef(IGC_METRICS::CodeRef* codeRef, llvm::DILocation* Loc)
    {
        if (Loc == nullptr || Loc->getDirectory().empty() || Loc->getFilename().empty())
        {
            return;
        }
        FillCodeRef(codeRef, GetFullPath(Loc->getDirectory().str(), Loc->getFilename().str()),
            Loc->getLine());
    }

    void IGCMetricImpl::FillCodeRef(IGC_METRICS::CodeRef* codeRef, llvm::DIVariable* Var)
    {
        if (Var == nullptr || Var->getDirectory().empty() || Var->getFilename().empty())
        {
            return;
        }
        FillCodeRef(codeRef, GetFullPath(Var->getDirectory().str(), Var->getFilename().str()),
            Var->getLine());
    }

    void IGCMetricImpl::FillCodeRef(IGC_METRICS::CodeRef* codeRef, const std::string& filePathName, int line)
    {
        if (filePathName.empty())
        {
            return;
        }
        codeRef->set_line(line);
        codeRef->set_pathtofile(filePathName);
    }

    const std::string IGCMetricImpl::GetFullPath(const char* dir, const char* fileName)
    {
        return GetFullPath(std::string(dir), std::string(fileName));
    }

    const std::string IGCMetricImpl::GetFullPath(const std::string& dir, const std::string& fileName)
    {
        llvm::SmallVector<char, 1024> fileNamebuf;
        llvm::sys::path::append(fileNamebuf, dir);
        llvm::sys::path::append(fileNamebuf, fileName);
        std::string fileNameStr(fileNamebuf.begin(), fileNamebuf.end());
        return fileNameStr;
    }

#endif

}
