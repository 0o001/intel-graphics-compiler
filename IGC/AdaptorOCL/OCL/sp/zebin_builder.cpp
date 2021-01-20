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

#include "zebin_builder.hpp"

#include "../../../Compiler/CodeGenPublic.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/ELF.h"
#include "Probe/Assertion.h"

using namespace IGC;
using namespace iOpenCL;
using namespace zebin;

ZEBinaryBuilder::ZEBinaryBuilder(
    const PLATFORM plat, bool is64BitPointer, const IGC::SOpenCLProgramInfo& programInfo,
    const uint8_t* spvData, uint32_t spvSize)
    : mPlatform(plat), mBuilder(is64BitPointer)
{
    G6HWC::InitializeCapsGen8(&mHWCaps);

    // IGC only generates executable
    mBuilder.setFileType(ELF_TYPE_ZEBIN::ET_ZEBIN_EXE);

    mBuilder.setMachine(plat.eProductFamily);

    // FIXME: Most fields leaves as 0
    TargetFlags tf;
    tf.generatorSpecificFlags = TargetFlags::GeneratorSpecificFlags::NONE;
    tf.minHwRevisionId = plat.usRevId;
    tf.maxHwRevisionId = plat.usRevId;
    tf.generatorId = TargetFlags::GeneratorId::IGC;
    mBuilder.setTargetFlag(tf);

    addProgramScopeInfo(programInfo);

    if (spvData != nullptr)
        addSPIRV(spvData, spvSize);
}

void ZEBinaryBuilder::setGfxCoreFamilyToELFMachine(uint32_t value)
{
    TargetFlags tf = mBuilder.getTargetFlag();
    tf.machineEntryUsesGfxCoreInsteadOfProductFamily = true;
    mBuilder.setTargetFlag(tf);
    mBuilder.setMachine(value);
}

void ZEBinaryBuilder::createKernel(
    const char*  rawIsaBinary,
    unsigned int rawIsaBinarySize,
    const SOpenCLKernelInfo& annotations,
    const uint32_t grfSize)
{
    ZEELFObjectBuilder::SectionID textID =
        addKernelBinary(annotations.m_kernelName, rawIsaBinary, rawIsaBinarySize);
    addSymbols(textID, annotations);
    addKernelRelocations(textID, annotations);

    zeInfoKernel& zeKernel = mZEInfoBuilder.createKernel(annotations.m_kernelName);
    addKernelExecEnv(annotations, zeKernel);
    addKernelExperimentalProperties(annotations, zeKernel);
    if (annotations.m_threadPayload.HasLocalIDx ||
        annotations.m_threadPayload.HasLocalIDy ||
        annotations.m_threadPayload.HasLocalIDz) {
        addLocalIds(annotations.m_executionEnivronment.CompiledSIMDSize,
            grfSize,
            annotations.m_threadPayload.HasLocalIDx,
            annotations.m_threadPayload.HasLocalIDy,
            annotations.m_threadPayload.HasLocalIDz,
            zeKernel);
    }
    addPayloadArgsAndBTI(annotations, zeKernel);
    addMemoryBuffer(annotations, zeKernel);
    addGTPinInfo(annotations);
}

void ZEBinaryBuilder::addGTPinInfo(const IGC::SOpenCLKernelInfo& annotations)
{
    const IGC::SKernelProgram* program = &(annotations.m_kernelProgram);
    uint8_t* buffer = nullptr;
    uint32_t size = 0;
    switch (annotations.m_executionEnivronment.CompiledSIMDSize) {
    case 1:
        buffer = (uint8_t*)program->simd1.m_gtpinBuffer;
        size = program->simd1.m_gtpinBufferSize;
        break;
    case 8:
        buffer = (uint8_t*)program->simd8.m_gtpinBuffer;
        size = program->simd8.m_gtpinBufferSize;
        break;
    case 16:
        buffer = (uint8_t*)program->simd16.m_gtpinBuffer;
        size = program->simd16.m_gtpinBufferSize;
        break;
    case 32:
        buffer = (uint8_t*)program->simd32.m_gtpinBuffer;
        size = program->simd32.m_gtpinBufferSize;
        break;
    }

    if (buffer != nullptr && size)
        mBuilder.addSectionGTPinInfo(annotations.m_kernelName, buffer, size);
}

void ZEBinaryBuilder::addProgramScopeInfo(const IGC::SOpenCLProgramInfo& programInfo)
{
    addGlobalConstants(programInfo);
    addGlobals(programInfo);
}

void ZEBinaryBuilder::addGlobalConstants(const IGC::SOpenCLProgramInfo& annotations)
{
    if (annotations.m_initConstantAnnotation.empty())
        return;

    // create a data section for global constant variables
    // Two constant data sections: general constants and string literals
    IGC_ASSERT(annotations.m_initConstantAnnotation.size() == 2);

    // General constants
    // create a data section for global constant variables
    auto& ca = annotations.m_initConstantAnnotation.front();
    if (ca->AllocSize) {
        // the normal .data.const size
        uint32_t dataSize = ca->InlineData.size();
        // the zero-initialize variables size, the .bss.const size
        uint32_t bssSize = ca->AllocSize - dataSize;
        uint32_t alignment = ca->Alignment;

        if (IGC_IS_FLAG_ENABLED(AllocateZeroInitializedVarsInBss)) {
            zebin::ZEELFObjectBuilder::SectionID normal_id = -1, bss_id = -1;
            if (dataSize) {
                // if the bss section existed, we leave the alignment in bss section.
                // that in our design the entire global buffer is the size of normal section (.const) plus bss section
                // we do not want to add the alignment twice on the both sections
                // Alos set the padding size to 0 that we always put the padding into bss section
                uint32_t normal_alignment = bssSize ? 0 : alignment;
                normal_id = mBuilder.addSectionData("const", (const uint8_t*)ca->InlineData.data(),
                    dataSize, 0, normal_alignment);
            }
            if (bssSize) {
                bss_id = mBuilder.addSectionBss("const", bssSize, alignment);
            }

            // set mGlobalConstSectID to normal_id if existed, and bss_id if not.
            // mGlobalConstSectID will be used for symbol section reference. We always refer to normal_id section
            // even if the the symbol is defeind in bss section when normal_id section exists
            mGlobalConstSectID = dataSize ? normal_id : bss_id;
        } else {
            // before runtime can support bss section, we create all 0s in .const.data section by adding
            // bssSize of padding
            mGlobalConstSectID = mBuilder.addSectionData("const", (const uint8_t*)ca->InlineData.data(),
                dataSize, bssSize, alignment);
        }
    }

    // String literals
    auto& caString = annotations.m_initConstantAnnotation[1];
    if (caString->InlineData.size() > 0)
    {
        uint32_t dataSize = caString->InlineData.size();
        uint32_t paddingSize = caString->AllocSize - dataSize;
        uint32_t alignment = caString->Alignment;
        mConstStringSectID = mBuilder.addSectionData("const.string", (const uint8_t*)caString->InlineData.data(),
            dataSize, paddingSize, alignment);
    }
}

void ZEBinaryBuilder::addGlobals(const IGC::SOpenCLProgramInfo& annotations)
{
    if (annotations.m_initGlobalAnnotation.empty())
        return;

    // create a data section for global variables
    // FIXME: not sure in what cases there will be more than one global buffer
    IGC_ASSERT(annotations.m_initGlobalAnnotation.size() == 1);
    auto& ca = annotations.m_initGlobalAnnotation.front();

    if (!ca->AllocSize)
        return;

    uint32_t dataSize = ca->InlineData.size();
    uint32_t bssSize = ca->AllocSize - dataSize;
    uint32_t alignment = ca->Alignment;

    if (IGC_IS_FLAG_ENABLED(AllocateZeroInitializedVarsInBss)) {
        // The .bss.global section size is the bssSize (ca->AllocSize - ca->InlineData.size()),
        // and the normal .data.global size is dataSize (ca->InlineData.size())
        zebin::ZEELFObjectBuilder::SectionID normal_id = -1, bss_id = -1;
        if (dataSize) {
            uint32_t normal_alignment = bssSize ? 0 : alignment;
            normal_id = mBuilder.addSectionData("global", (const uint8_t*)ca->InlineData.data(),
                dataSize, 0, normal_alignment);
        }
        if (bssSize) {
            bss_id = mBuilder.addSectionBss("global", bssSize, alignment);
        }
        // mGlobalSectID is the section id that will be referenced by global symbols.
        // It should be .data.global if existed. If there's only .bss.global section, then all global
        // symbols reference to .bss.global section, so set the mGlobalConstSectID to it
        mGlobalSectID = dataSize ? normal_id : bss_id;
    } else {
        // before runtime can support bss section, we create all 0s in .global.data section by adding
        // bssSize of padding
        mGlobalSectID = mBuilder.addSectionData("global", (const uint8_t*)ca->InlineData.data(),
            dataSize, bssSize, alignment);
    }
}

void ZEBinaryBuilder::addSPIRV(const uint8_t* data, uint32_t size)
{
    mBuilder.addSectionSpirv("", data, size);
}

ZEELFObjectBuilder::SectionID ZEBinaryBuilder::addKernelBinary(const std::string& kernelName,
    const char* kernelBinary, unsigned int kernelBinarySize)
{
    return mBuilder.addSectionText(kernelName, (const uint8_t*)kernelBinary,
        kernelBinarySize, mHWCaps.InstructionCachePrefetchSize, sizeof(DWORD));
}

void ZEBinaryBuilder::addPayloadArgsAndBTI(
    const SOpenCLKernelInfo& annotations,
    zeInfoKernel& zeinfoKernel)
{
    // copy the payload arguments into zeinfoKernel
    zeinfoKernel.payload_arguments.insert(
        zeinfoKernel.payload_arguments.end(),
        annotations.m_zePayloadArgs.begin(),
        annotations.m_zePayloadArgs.end());

    // copy the bit table into zeinfoKernel
    zeinfoKernel.binding_table_indices.insert(
        zeinfoKernel.binding_table_indices.end(),
        annotations.m_zeBTIArgs.begin(),
        annotations.m_zeBTIArgs.end());
}

void ZEBinaryBuilder::addMemoryBuffer(
    const IGC::SOpenCLKernelInfo& annotations,
    zebin::zeInfoKernel& zeinfoKernel)
{
    // scracth0 is either
    //  - contains privates and both igc and vISA stack, or
    //  - contains only vISA stack
    uint32_t scratch0 =
        annotations.m_executionEnivronment.PerThreadScratchSpace;
    // scratch1 is privates on stack
    uint32_t scratch1 =
        annotations.m_executionEnivronment.PerThreadScratchSpaceSlot1;
    // private_on_global: privates and IGC stack on stateless
    uint32_t private_on_global =
        annotations.m_executionEnivronment.PerThreadPrivateOnStatelessSize;

    //  single scratch space have everything
    if (scratch0 && !scratch1 && !private_on_global) {
        ZEInfoBuilder::addPerThreadMemoryBuffer(zeinfoKernel.per_thread_memory_buffers,
            PreDefinedAttrGetter::MemBufferType::scratch,
            PreDefinedAttrGetter::MemBufferUsage::single_space,
            scratch0);

        return;
    }

    if (scratch0)
        ZEInfoBuilder::addScratchPerThreadMemoryBuffer(zeinfoKernel.per_thread_memory_buffers,
            PreDefinedAttrGetter::MemBufferUsage::spill_fill_space,
            0,
            scratch0);
    if (scratch1)
        ZEInfoBuilder::addScratchPerThreadMemoryBuffer(zeinfoKernel.per_thread_memory_buffers,
            PreDefinedAttrGetter::MemBufferUsage::private_space,
            1,
            scratch1);
    if (private_on_global) {
        ZEInfoBuilder::addPerSIMTThreadGlobalMemoryBuffer(zeinfoKernel.per_thread_memory_buffers,
            PreDefinedAttrGetter::MemBufferUsage::private_space,
            private_on_global);
        // FIXME: IGC currently generate global buffer with size assume to be per-simt-thread
        // ZEInfoBuilder::addPerThreadMemoryBuffer(zeinfoKernel.per_thread_memory_buffers,
        //    PreDefinedAttrGetter::MemBufferType::global,
        //    PreDefinedAttrGetter::MemBufferUsage::private_space,
        //    private_on_global);
    }
}

uint8_t ZEBinaryBuilder::getSymbolElfType(vISA::ZESymEntry& sym)
{
    switch (sym.s_type) {
    case vISA::GenSymType::S_NOTYPE:
        return llvm::ELF::STT_NOTYPE;

    case vISA::GenSymType::S_UNDEF:
        return llvm::ELF::STT_NOTYPE;

    case vISA::GenSymType::S_FUNC:
    case vISA::GenSymType::S_KERNEL:
        return llvm::ELF::STT_FUNC;

    case vISA::GenSymType::S_GLOBAL_VAR:
    case vISA::GenSymType::S_GLOBAL_VAR_CONST:
    case vISA::GenSymType::S_CONST_SAMPLER:
        return llvm::ELF::STT_OBJECT;
    default:
        break;
    }
    return llvm::ELF::STT_NOTYPE;
}

uint8_t ZEBinaryBuilder::getSymbolElfBinding(vISA::ZESymEntry& sym)
{
    // all symbols we have now that could be exposed must have
    // global binding
    switch (sym.s_type) {
    case vISA::GenSymType::S_KERNEL:
        return llvm::ELF::STB_LOCAL;

    case vISA::GenSymType::S_NOTYPE:
    case vISA::GenSymType::S_UNDEF:
    case vISA::GenSymType::S_FUNC:
    case vISA::GenSymType::S_GLOBAL_VAR:
    case vISA::GenSymType::S_GLOBAL_VAR_CONST:
    case vISA::GenSymType::S_CONST_SAMPLER:
        return llvm::ELF::STB_GLOBAL;
    default:
        break;
    }
    IGC_ASSERT(0);
    return llvm::ELF::STB_GLOBAL;
}

void ZEBinaryBuilder::addSymbols(
    ZEELFObjectBuilder::SectionID kernelSectId,
    const IGC::SOpenCLKernelInfo& annotations)
{
    // get symbol list from the current process SKernelProgram
    auto symbols = [](int simdSize, const IGC::SKernelProgram& program) {
        if (simdSize == 8)
            return program.simd8.m_symbols;
        else if (simdSize == 16)
            return program.simd16.m_symbols;
        else if (simdSize == 32)
            return program.simd32.m_symbols;
        else
            return program.simd1.m_symbols;
    } (annotations.m_executionEnivronment.CompiledSIMDSize,
        annotations.m_kernelProgram);

    // add local symbols of this kernel binary
    for (auto sym : symbols.local) {
        IGC_ASSERT(sym.s_type != vISA::GenSymType::S_UNDEF);
        mBuilder.addSymbol(sym.s_name, sym.s_offset, sym.s_size,
            getSymbolElfBinding(sym), getSymbolElfType(sym), kernelSectId);
    }

    // If the symbol has UNDEF type, set its sectionId to -1
    // add function symbols defined in kernel text
    for (auto sym : symbols.function)
        mBuilder.addSymbol(sym.s_name, sym.s_offset, sym.s_size,
            getSymbolElfBinding(sym), getSymbolElfType(sym),
            (sym.s_type == vISA::GenSymType::S_UNDEF) ? -1 : kernelSectId);

    // add symbols defined in global constant section
    for (auto sym : symbols.globalConst)
        mBuilder.addSymbol(sym.s_name, sym.s_offset, sym.s_size,
            getSymbolElfBinding(sym), getSymbolElfType(sym),
            (sym.s_type == vISA::GenSymType::S_UNDEF) ? -1 : mGlobalConstSectID);

    // add symbols defined in global section
    for (auto sym : symbols.global)
        mBuilder.addSymbol(sym.s_name, sym.s_offset, sym.s_size,
            getSymbolElfBinding(sym), getSymbolElfType(sym),
            (sym.s_type == vISA::GenSymType::S_UNDEF) ? -1 : mGlobalSectID);

    // we do not support sampler symbols now
    IGC_ASSERT(symbols.sampler.empty());
}

void ZEBinaryBuilder::addKernelRelocations(
    ZEELFObjectBuilder::SectionID targetId,
    const IGC::SOpenCLKernelInfo& annotations)
{
    // get relocation list from the current process SKernelProgram
    auto relocs = [](int simdSize, const IGC::SKernelProgram& program) {
        if (simdSize == 8)
            return program.simd8.m_relocs;
        else if (simdSize == 16)
            return program.simd16.m_relocs;
        else if (simdSize == 32)
            return program.simd32.m_relocs;
        else
            return program.simd1.m_relocs;
    } (annotations.m_executionEnivronment.CompiledSIMDSize, annotations.m_kernelProgram);

    // FIXME: For r_type, zebin::R_TYPE_ZEBIN should have the same enum value as visa::GenRelocType.
    // Take the value directly
    if (!relocs.empty())
        for (auto reloc : relocs)
            mBuilder.addRelocation(reloc.r_offset, reloc.r_symbol, (zebin::R_TYPE_ZEBIN)reloc.r_type, targetId);
}

void ZEBinaryBuilder::addKernelExperimentalProperties(const SOpenCLKernelInfo& annotations,
    zeInfoKernel& zeinfoKernel)
{
    // Write to zeinfoKernel only when the attribute is enabled
    if (IGC_IS_FLAG_ENABLED(DumpHasNonKernelArgLdSt)) {
        ZEInfoBuilder::addExpPropertiesHasNonKernelArgLdSt(zeinfoKernel,
            annotations.m_hasNonKernelArgLoad,
            annotations.m_hasNonKernelArgStore,
            annotations.m_hasNonKernelArgAtomic);
    }
}

void ZEBinaryBuilder::addKernelExecEnv(const SOpenCLKernelInfo& annotations,
    zeInfoKernel& zeinfoKernel)
{
    zeInfoExecutionEnv& env = zeinfoKernel.execution_env;

    // FIXME: compiler did not provide this information
    env.actual_kernel_start_offset = 0;

    env.barrier_count = annotations.m_executionEnivronment.HasBarriers;
    env.disable_mid_thread_preemption = annotations.m_executionEnivronment.DisableMidThreadPreemption;
    env.grf_count = annotations.m_executionEnivronment.NumGRFRequired;
    env.has_4gb_buffers = annotations.m_executionEnivronment.CompiledForGreaterThan4GBBuffers;
    env.has_device_enqueue = annotations.m_executionEnivronment.HasDeviceEnqueue;
    env.has_fence_for_image_access = annotations.m_executionEnivronment.HasReadWriteImages;
    env.has_global_atomics = annotations.m_executionEnivronment.HasGlobalAtomics;
    env.offset_to_skip_per_thread_data_load = annotations.m_threadPayload.OffsetToSkipPerThreadDataLoad;;
    env.offset_to_skip_set_ffid_gp = annotations.m_threadPayload.OffsetToSkipSetFFIDGP;;
    env.required_sub_group_size = annotations.m_executionEnivronment.CompiledSubGroupsNumber;
    if(annotations.m_executionEnivronment.HasFixedWorkGroupSize)
    {
        env.required_work_group_size.push_back(annotations.m_executionEnivronment.FixedWorkgroupSize[0]);
        env.required_work_group_size.push_back(annotations.m_executionEnivronment.FixedWorkgroupSize[1]);
        env.required_work_group_size.push_back(annotations.m_executionEnivronment.FixedWorkgroupSize[2]);
    }
    env.simd_size = annotations.m_executionEnivronment.CompiledSIMDSize;
    // set slm size to inline local size
    env.slm_size = annotations.m_executionEnivronment.SumFixedTGSMSizes ;
    env.subgroup_independent_forward_progress = annotations.m_executionEnivronment.SubgroupIndependentForwardProgressRequired;
    if (annotations.m_executionEnivronment.WorkgroupWalkOrder[0] ||
        annotations.m_executionEnivronment.WorkgroupWalkOrder[1] ||
        annotations.m_executionEnivronment.WorkgroupWalkOrder[2]) {
        env.work_group_walk_order_dimensions.push_back(annotations.m_executionEnivronment.WorkgroupWalkOrder[0]);
        env.work_group_walk_order_dimensions.push_back(annotations.m_executionEnivronment.WorkgroupWalkOrder[1]);
        env.work_group_walk_order_dimensions.push_back(annotations.m_executionEnivronment.WorkgroupWalkOrder[2]);
    }
}

void ZEBinaryBuilder::addLocalIds(uint32_t simdSize, uint32_t grfSize,
    bool has_local_id_x, bool has_local_id_y, bool has_local_id_z,
    zebin::zeInfoKernel& zeinfoKernel)
{
    // simdSize 1 is CM kernel, using arg_type::packed_local_ids format
    if (simdSize == 1) {
        // Currently there's only one kind of per-thread argument, hard-coded the
        // offset to 0 and for packed_local_ids, its size is 6 bytes (int16*3) always
        mZEInfoBuilder.addPerThreadPayloadArgument(
            zeinfoKernel.per_thread_payload_arguments,
            PreDefinedAttrGetter::ArgType::packed_local_ids, 0, 6);
        return;
    }
    // otherwise, using arg_type::local_id format
    IGC_ASSERT(simdSize);
    IGC_ASSERT(grfSize);
    // each id takes 2 bytes
    int32_t per_id_size = 2 * simdSize;
    // byte size for one id have to be grf align
    per_id_size = (per_id_size % grfSize) == 0 ?
        per_id_size : ((per_id_size / grfSize) + 1) * grfSize;
    // total_size = num_of_ids * per_id_size
    int32_t total_size = per_id_size * ((has_local_id_x ? 1 : 0) +
        (has_local_id_y ? 1 : 0) + (has_local_id_z ? 1 : 0));
    mZEInfoBuilder.addPerThreadPayloadArgument(
        zeinfoKernel.per_thread_payload_arguments,
        PreDefinedAttrGetter::ArgType::local_id, 0, total_size);
}

void ZEBinaryBuilder::getBinaryObject(llvm::raw_pwrite_stream& os)
{
    mBuilder.addSectionZEInfo(mZEInfoBuilder.getZEInfoContainer());
    mBuilder.finalize(os);
}

void ZEBinaryBuilder::getBinaryObject(Util::BinaryStream& outputStream)
{
    llvm::SmallVector<char, 64> buf;
    llvm::raw_svector_ostream llvm_os(buf);
    getBinaryObject(llvm_os);
    outputStream.Write(buf.data(), buf.size());
}

void ZEBinaryBuilder::printBinaryObject(const std::string& filename)
{
    std::error_code EC;
    llvm::raw_fd_ostream os(filename, EC);
    mBuilder.finalize(os);
    os.close();
}
