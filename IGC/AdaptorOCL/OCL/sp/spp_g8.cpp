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

#include "spp_g8.h"

#include "../../../Compiler/CodeGenPublic.h"
#include "program_debug_data.h"
#include "../../../common/Types.hpp"
#include "../../../Compiler/CISACodeGen/OpenCLKernelCodeGen.hpp"

namespace iOpenCL
{

extern RETVAL g_cInitRetValue;

CGen8OpenCLProgram::CGen8OpenCLProgram(PLATFORM platform, IGC::OpenCLProgramContext &context) :
    m_StateProcessor( platform, context ),
    m_Platform( platform ),
    m_pContext( &context )
{
    m_ProgramScopePatchStream = new Util::BinaryStream();
}

CGen8OpenCLProgram::~CGen8OpenCLProgram()
{
    delete m_ProgramScopePatchStream;

    for (auto data : m_KernelBinaries)
    {
        delete data.kernelBinary;
        delete data.kernelDebugData;
    }

    for (auto p : m_ShaderProgramList)
    {
        delete p.second;
    }
    m_ShaderProgramList.clear();

    delete m_pSystemThreadKernelOutput;
    m_pSystemThreadKernelOutput = nullptr;
}

RETVAL CGen8OpenCLProgram::GetProgramBinary(
    Util::BinaryStream& programBinary,
    unsigned int pointerSizeInBytes )
{
    RETVAL retValue = g_cInitRetValue;

    iOpenCL::SProgramBinaryHeader   header;

    memset( &header, 0, sizeof( header ) );

    header.Magic = iOpenCL::MAGIC_CL;
    header.Version = iOpenCL::CURRENT_ICBE_VERSION;
    header.Device = m_Platform.eRenderCoreFamily;
    header.GPUPointerSizeInBytes = pointerSizeInBytes;
    header.NumberOfKernels = m_KernelBinaries.size();
    header.SteppingId = m_Platform.usRevId;
    header.PatchListSize = int_cast<DWORD>(m_ProgramScopePatchStream->Size());
    
    if (IGC_IS_FLAG_ENABLED(DumpOCLProgramInfo))
    {
        DebugProgramBinaryHeader(&header, m_StateProcessor.m_oclStateDebugMessagePrintOut);
    }

    programBinary.Write( header );

    programBinary.Write( *m_ProgramScopePatchStream );

    for( auto data : m_KernelBinaries )
    {
        programBinary.Write( *(data.kernelBinary) );
    }

    return retValue;
}

RETVAL CGen8OpenCLProgram::GetProgramDebugData(Util::BinaryStream& programDebugData)
{
    RETVAL retValue = g_cInitRetValue;

    unsigned numDebugBinaries = 0;
    for (auto data : m_KernelBinaries)
    {
        if (data.kernelDebugData && data.kernelDebugData->Size() > 0)
        {
            numDebugBinaries++;
        }
    }

    if( numDebugBinaries )
    {
        iOpenCL::SProgramDebugDataHeaderIGC header;

        memset( &header, 0, sizeof( header ) );

        header.Magic = iOpenCL::MAGIC_CL;
        header.Version = iOpenCL::CURRENT_ICBE_VERSION;
        header.Device = m_Platform.eRenderCoreFamily;
        header.NumberOfKernels = numDebugBinaries;
        header.SteppingId = m_Platform.usRevId;

        programDebugData.Write( header );

        for (auto data : m_KernelBinaries)
        {
            if (data.kernelDebugData && data.kernelDebugData->Size() > 0)
            {
                programDebugData.Write( *(data.kernelDebugData) );
            }
        }
    }

    return retValue;
}

void CGen8OpenCLProgram::CreateKernelBinaries()
{
    auto isValidShader = [&](IGC::COpenCLKernel* shader)->bool
    {
        return (shader && shader->ProgramOutput()->m_programSize > 0);
    };

    for (auto pKernel : m_ShaderProgramList)
    {
        IGC::COpenCLKernel* simd8Shader = static_cast<IGC::COpenCLKernel*>(pKernel.second->GetShader(SIMDMode::SIMD8));
        IGC::COpenCLKernel* simd16Shader = static_cast<IGC::COpenCLKernel*>(pKernel.second->GetShader(SIMDMode::SIMD16));
        IGC::COpenCLKernel* simd32Shader = static_cast<IGC::COpenCLKernel*>(pKernel.second->GetShader(SIMDMode::SIMD32));

        // Determine how many simd modes we have per kernel
        std::vector<IGC::COpenCLKernel*> kernelVec;
        if (m_pContext->m_DriverInfo.sendMultipleSIMDModes() && (m_pContext->getModuleMetaData()->csInfo.forcedSIMDSize == 0))
        {
            SIMDMode defaultSIMD = m_pContext->getDefaultSIMDMode();
            switch (defaultSIMD)
            {
                case SIMDMode::SIMD32:
                    if (isValidShader(simd32Shader))
                        kernelVec.push_back(simd32Shader);
                    break;
                case SIMDMode::SIMD16:
                    if (isValidShader(simd16Shader))
                        kernelVec.push_back(simd16Shader);
                    break;
                case SIMDMode::SIMD8:
                    if (isValidShader(simd8Shader))
                        kernelVec.push_back(simd8Shader);
                    break;
                default:
                    assert(0 && "SIMD must be 32/16/8");
            }

            // Push remaining simd modes
            if (defaultSIMD != SIMDMode::SIMD32 && isValidShader(simd32Shader))
                kernelVec.push_back(simd32Shader);
            if (defaultSIMD != SIMDMode::SIMD16 && isValidShader(simd16Shader))
                kernelVec.push_back(simd16Shader);
            if (defaultSIMD != SIMDMode::SIMD8 && isValidShader(simd8Shader))
                kernelVec.push_back(simd8Shader);
        }
        else
        {
            if (isValidShader(simd32Shader))
                kernelVec.push_back(simd32Shader);
            else if (isValidShader(simd16Shader))
                kernelVec.push_back(simd16Shader);
            else if (isValidShader(simd8Shader))
                kernelVec.push_back(simd8Shader);
        }

        for (auto kernel : kernelVec)
        {
            IGC::SProgramOutput* pOutput = kernel->ProgramOutput();

            // Create the kernel binary streams
            KernelData data;
            data.kernelBinary = new Util::BinaryStream();

            m_StateProcessor.CreateKernelBinary(
                (const char*)pOutput->m_programBin,
                pOutput->m_programSize,
                kernel->m_kernelInfo,
                m_pContext->m_programInfo,
                m_pContext->btiLayout,
                *(data.kernelBinary),
                m_pSystemThreadKernelOutput,
                pOutput->m_unpaddedProgramSize);

            assert(data.kernelBinary && data.kernelBinary->Size() > 0);

            // Create the debug data binary streams
            if (pOutput->m_debugDataVISASize > 0 && pOutput->m_debugDataGenISASize > 0)
            {
                data.kernelDebugData = new Util::BinaryStream();

                m_StateProcessor.CreateKernelDebugData(
                    (const char*)pOutput->m_debugDataVISA,
                    pOutput->m_debugDataVISASize,
                    (const char*)pOutput->m_debugDataGenISA,
                    pOutput->m_debugDataGenISASize,
                    kernel->m_kernelInfo.m_kernelName,
                    *(data.kernelDebugData));
            }

            m_KernelBinaries.push_back(data);
        }
    }
}

void CGen8OpenCLProgram::CreateProgramScopePatchStream(const IGC::SOpenCLProgramInfo& annotations)
{
    m_StateProcessor.CreateProgramScopePatchStream(annotations, *m_ProgramScopePatchStream);
}

}
