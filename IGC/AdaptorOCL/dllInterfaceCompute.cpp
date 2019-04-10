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

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/ScaledNumber.h>
#include "common/LLVMWarningsPop.hpp"

#include <assert.h>
#include <cstring>
#include <string>
#include <stdexcept>
#include <fstream>

#include "AdaptorCommon/customApi.hpp"
#include "AdaptorOCL/OCL/LoadBuffer.h"
#include "AdaptorOCL/OCL/BuiltinResource.h"
#include "AdaptorOCL/OCL/TB/igc_tb.h"

#include "AdaptorOCL/Upgrader/Upgrader.h"
#include "AdaptorOCL/UnifyIROCL.hpp"
#include "AdaptorOCL/DriverInfoOCL.hpp"

#include "Compiler/MetaDataApi/IGCMetaDataHelper.h"
#include "common/debug/Dump.hpp"
#include "common/debug/Debug.hpp"
#include "common/igc_regkeys.hpp"
#include "common/secure_mem.h"
#include "common/shaderOverride.hpp"

#include "CLElfLib/ElfReader.h"
#include "usc.h"

#include "AdaptorOCL/OCL/sp/gtpin_igc_ocl.h"
#include "AdaptorOCL/igcmc.h"
#include "RT_Jitter_Interface.h"

#include <iStdLib/MemCopy.h>


#if defined(IGC_SPIRV_ENABLED)
#include "common/LLVMWarningsPush.hpp"
#include "AdaptorOCL/SPIRV/SPIRVconsum.h"
#include "common/LLVMWarningsPop.hpp"
#include "AdaptorOCL/SPIRV/libSPIRV/SPIRVModule.h"
#include "AdaptorOCL/SPIRV/libSPIRV/SPIRVValue.h"
#endif

#include "common/LLVMWarningsPush.hpp"
#include "llvmWrapper/Bitcode/BitcodeWriter.h"
#include "common/LLVMWarningsPop.hpp"

#include <sstream>
#include <iomanip>


//In case of use GT_SYSTEM_INFO in GlobalData.h from inc/umKmInc/sharedata.h
//We have to do this temporary defines

#ifdef BOOLEAN
#define BOOLEAN_IGC_REPLACED
#pragma push_macro("BOOLEAN")
#undef BOOLEAN
#endif
#define BOOLEAN uint8_t

#ifdef HANDLE
#define HANDLE_IGC_REPLACED
#pragma push_macro("HANDLE")
#undef HANDLE
#endif
#define HANDLE void*

#ifdef VER_H
#define VER_H_IGC_REPLACED
#pragma push_macro("VER_H")
#undef VER_H
#endif
#define VER_H

#include "GlobalData.h"

//We undef BOOLEAN HANDLE and VER_H here
#undef VER_H
#ifdef VER_H_IGC_REPLACED
#pragma pop_macro("VER_H")
#undef VER_H_IGC_REPLACED
#endif

#undef BOOLEAN
#ifdef BOOLEAN_IGC_REPLACED
#pragma pop_macro("BOOLEAN")
#undef BOOLEAN_IGC_REPLACED
#endif

#undef HANDLE
#ifdef HANDLE_IGC_REPLACED
#pragma pop_macro("HANDLE")
#undef HANDLE_IGC_REPLACED
#endif

#if !defined(_WIN32)
#   define strtok_s strtok_r
#   define _strdup strdup
#   define _snprintf snprintf
#endif


#include "common/LLVMWarningsPush.hpp"
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Linker/Linker.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include "common/LLVMWarningsPop.hpp"

using namespace IGC::IGCMD;
using namespace IGC::Debug;
using namespace IGC;


namespace TC
{

extern bool ProcessElfInput(
  STB_TranslateInputArgs &InputArgs,
  STB_TranslateOutputArgs &OutputArgs,
  IGC::OpenCLProgramContext &Context,
  PLATFORM &platform, bool isOutputLlvmBinary);
  
extern bool ParseInput(
  llvm::Module*& pKernelModule,
  const STB_TranslateInputArgs* pInputArgs,
  STB_TranslateOutputArgs* pOutputArgs,
  IGC::OpenCLProgramContext &oclContext,
  TB_DATA_FORMAT inputDataFormatTemp);
    
bool TranslateBuild(
    const STB_TranslateInputArgs* pInputArgs,
    STB_TranslateOutputArgs* pOutputArgs,
    TB_DATA_FORMAT inputDataFormatTemp,
    const IGC::CPlatform& IGCPlatform,
    float profilingTimerResolution);

bool CIGCTranslationBlock::ProcessElfInput(
  STB_TranslateInputArgs &InputArgs,
  STB_TranslateOutputArgs &OutputArgs,
  IGC::OpenCLProgramContext &Context){
    return TC::ProcessElfInput(InputArgs, OutputArgs, Context, m_Platform, m_DataFormatOutput == TB_DATA_FORMAT_LLVM_BINARY);
}

CIGCTranslationBlock::CIGCTranslationBlock()
{

}

CIGCTranslationBlock::~CIGCTranslationBlock()
{

}

static void SetErrorMessage(const std::string & ErrorMessage, STB_TranslateOutputArgs & pOutputArgs)
{
    pOutputArgs.pErrorString = new char[ErrorMessage.size() + 1];
    memcpy_s(pOutputArgs.pErrorString, ErrorMessage.size() + 1, ErrorMessage.c_str(), ErrorMessage.size() + 1);
    pOutputArgs.ErrorStringSize = ErrorMessage.size() + 1;
}

bool CIGCTranslationBlock::Create(
    const STB_CreateArgs* pCreateArgs,
    CIGCTranslationBlock* &pTranslationBlock )
{
    bool    success = true;

    pTranslationBlock = new CIGCTranslationBlock();

    if( pTranslationBlock )
    {
        success = pTranslationBlock->Initialize(pCreateArgs );

        if( !success )
        {
            CIGCTranslationBlock::Delete( pTranslationBlock );
        }
    }
    else
    {
        success = false;
    }

    return success;
}

void CIGCTranslationBlock::Delete(
    CIGCTranslationBlock* &pTranslationBlock )
{
    delete pTranslationBlock;
    pTranslationBlock = NULL;
}

bool CIGCTranslationBlock::Translate(
    const STB_TranslateInputArgs* pInputArgs,
    STB_TranslateOutputArgs* pOutputArgs )
{
  // Create a copy of input arguments that can be modified
    STB_TranslateInputArgs InputArgsCopy = *pInputArgs;
    
    IGC::CPlatform IGCPlatform(m_Platform);

    SUscGTSystemInfo gtSystemInfo = { 0 };
    gtSystemInfo.EUCount = m_SysInfo.EUCount;
    gtSystemInfo.ThreadCount = m_SysInfo.ThreadCount;
    gtSystemInfo.SliceCount = m_SysInfo.SliceCount;
    gtSystemInfo.SubSliceCount = m_SysInfo.SubSliceCount;
    gtSystemInfo.IsDynamicallyPopulated = m_SysInfo.IsDynamicallyPopulated;
    gtSystemInfo.TotalVsThreads = m_SysInfo.TotalVsThreads;
    gtSystemInfo.TotalPsThreadsWindowerRange = m_SysInfo.TotalPsThreadsWindowerRange;
    gtSystemInfo.TotalDsThreads = m_SysInfo.TotalDsThreads;
    gtSystemInfo.TotalGsThreads = m_SysInfo.TotalGsThreads;
    gtSystemInfo.TotalHsThreads = m_SysInfo.TotalHsThreads;
    gtSystemInfo.MaxEuPerSubSlice = m_SysInfo.MaxEuPerSubSlice;
    gtSystemInfo.EuCountPerPoolMax = m_SysInfo.EuCountPerPoolMax;
    
    IGC::SetGTSystemInfo(&gtSystemInfo, &IGCPlatform);
    IGC::SetWorkaroundTable(&m_SkuTable, &IGCPlatform);
    IGC::SetCompilerCaps(&m_SkuTable, &IGCPlatform);

    pOutputArgs->pOutput = nullptr;
    pOutputArgs->OutputSize = 0;
    pOutputArgs->pErrorString = nullptr;
    pOutputArgs->ErrorStringSize = 0;
    pOutputArgs->pDebugData = nullptr;
    pOutputArgs->DebugDataSize = 0;


    LoadRegistryKeys();

    if (m_DataFormatInput == TB_DATA_FORMAT_ELF)
    {
        // Handle TB_DATA_FORMAT_ELF input as a result of a call to
        // clLinkLibrary(). There are two possible scenarios, link input
        // to form a new library (BC module) or link input to form an
        // executable.
            
        // First, link input modules together
        USC::SShaderStageBTLayout zeroLayout = USC::g_cZeroShaderStageBTLayout;
        IGC::COCLBTILayout oclLayout(&zeroLayout);
        CDriverInfoOCLNEO driverInfo;
        IGC::OpenCLProgramContext oclContextTemp(oclLayout, IGCPlatform, &InputArgsCopy, driverInfo, nullptr,
                                                 m_DataFormatOutput == TC::TB_DATA_FORMAT_NON_COHERENT_DEVICE_BINARY);
        RegisterComputeErrHandlers(*oclContextTemp.getLLVMContext());
        bool success = ProcessElfInput(InputArgsCopy, *pOutputArgs, oclContextTemp);

        return success;
    }

    if ((m_DataFormatInput == TB_DATA_FORMAT_LLVM_TEXT) ||
        (m_DataFormatInput == TB_DATA_FORMAT_SPIR_V) ||
        (m_DataFormatInput == TB_DATA_FORMAT_LLVM_BINARY))
    {
        return TC::TranslateBuild(&InputArgsCopy, pOutputArgs, m_DataFormatInput, IGCPlatform, m_ProfilingTimerResolution);
    }
    else
    {
        assert(0 && "Unsupported input format");
        return false;
    }
    return false;
}

std::unordered_map<uint32_t, uint64_t> UnpackSpecConstants(
    const uint32_t* pSpecConstantsIds,
    const uint64_t* pSpecConstantsValues,
    uint32_t size)
{
    std::unordered_map<uint32_t, uint64_t> outSpecConstantsMap;
    for (uint32_t i = 0; i < size; i++)
    {
        outSpecConstantsMap[pSpecConstantsIds[i]] = pSpecConstantsValues[i];
    }
    return outSpecConstantsMap;
}

bool ProcessElfInput(
  STB_TranslateInputArgs &InputArgs,
  STB_TranslateOutputArgs &OutputArgs,
  IGC::OpenCLProgramContext &Context,
  PLATFORM &platform,
  bool isOutputLlvmBinary)
{
  bool success = true;
  std::string ErrorMsg;

    CLElfLib::CElfReader *pElfReader = CLElfLib::CElfReader::Create(InputArgs.pInput, InputArgs.InputSize);
    CLElfLib::RAIIElf X(pElfReader); // When going out of scope this object calls the Delete() function automatically

    // If input buffer is an ELF file, then process separately
    const CLElfLib::SElf64Header* pHeader = pElfReader->GetElfHeader();
    if (pHeader != NULL)
    {
      // Create an empty module to store the output
      std::unique_ptr<llvm::Module> OutputModule;

      // Iterate over all the input modules.
      for (unsigned i = 1; i < pHeader->NumSectionHeaderEntries; i++)
      {
        const CLElfLib::SElf64SectionHeader* pSectionHeader = pElfReader->GetSectionHeader(i);
        assert(pSectionHeader != NULL);

        char* pData = NULL;
        size_t dataSize = 0;

        if (pSectionHeader->Type == CLElfLib::SH_TYPE_SPIRV_SC_IDS)
        {
            pElfReader->GetSectionData(i, pData, dataSize);
            InputArgs.pSpecConstantsIds = reinterpret_cast<const uint32_t*>(pData);
        }

        if (pSectionHeader->Type == CLElfLib::SH_TYPE_SPIRV_SC_VALUES)
        {
            pElfReader->GetSectionData(i, pData, dataSize);
            InputArgs.pSpecConstantsValues = reinterpret_cast<const uint64_t*>(pData);
        }

        if ((pSectionHeader->Type == CLElfLib::SH_TYPE_OPENCL_LLVM_BINARY)  ||
            (pSectionHeader->Type == CLElfLib::SH_TYPE_OPENCL_LLVM_ARCHIVE) ||
            (pSectionHeader->Type == CLElfLib::SH_TYPE_SPIRV))
        {
          pElfReader->GetSectionData(i, pData, dataSize);

          // Create input module from the buffer
          llvm::StringRef buf(pData, dataSize);

          std::unique_ptr<llvm::Module> InputModule = nullptr;

          if (pSectionHeader->Type == CLElfLib::SH_TYPE_SPIRV)
          {
              llvm::Module* pKernelModule = nullptr;
#if defined(IGC_SPIRV_ENABLED)
              Context.setAsSPIRV();
              std::istringstream IS(buf);
              std::string stringErrMsg;
              llvm::StringRef options;
              if(InputArgs.OptionsSize > 0){
                  options = llvm::StringRef(InputArgs.pOptions, InputArgs.OptionsSize - 1);
              }
              std::unordered_map<uint32_t, uint64_t> specIDToSpecValueMap = UnpackSpecConstants(
                                                                                  InputArgs.pSpecConstantsIds,
                                                                                  InputArgs.pSpecConstantsValues,
                                                                                  InputArgs.SpecConstantsSize);
              bool success = spv::ReadSPIRV(*Context.getLLVMContext(), IS, pKernelModule, options, stringErrMsg, &specIDToSpecValueMap);
#else
              std::string stringErrMsg{ "SPIRV consumption not enabled for the TARGET." };
              bool success = false;
#endif
              if (success)
              {
                  InputModule.reset(pKernelModule);
              }
          }
          else
          {
              std::unique_ptr<llvm::MemoryBuffer> pInputBuffer =
                  llvm::MemoryBuffer::getMemBuffer(buf, "", false);

              llvm::Expected<std::unique_ptr<llvm::Module>> errorOrModule = 
                    llvm::parseBitcodeFile(pInputBuffer->getMemBufferRef(), *Context.getLLVMContext());
              if (llvm::Error EC = errorOrModule.takeError())
              {
                  std::string errMsg;
                  llvm::handleAllErrors(std::move(EC), [&](llvm::ErrorInfoBase &EIB) {
                      llvm::SMDiagnostic(pInputBuffer->getBufferIdentifier(), llvm::SourceMgr::DK_Error,
                          EIB.message());
                  });
                  assert(errMsg.empty() && "parsing bitcode failed");
              }

              InputModule = std::move(errorOrModule.get());
          }

          if (InputModule.get() == NULL)
          {
              success = false;
              break;
          }

          // Link modules
          if (OutputModule.get() == NULL)
          {
              InputModule.swap(OutputModule);
          }
          else
          {
              success = !llvm::Linker::linkModules(*OutputModule, std::move(InputModule));
          }

          if (!success)
          {
              break;
          }
        }
      }

      if (success == true)
      {
        // Now that the output modules are linked the resulting module needs to be
        // serialized out
        std::string OutputString;
        llvm::raw_string_ostream OStream(OutputString);
        IGCLLVM::WriteBitcodeToFile(OutputModule.get(), OStream);
        OStream.flush();

        // Create a copy of the string to return to the caller. The output type
        // determines how the buffer gets managed
        char *pBufResult = static_cast<char*>(operator new(OutputString.size(), std::nothrow));
        if (pBufResult != NULL)
        {
          memcpy_s(pBufResult, OutputString.size(), OutputString.c_str(), OutputString.size());

          if (isOutputLlvmBinary)
          {
            // The buffer is returned to the runtime. When the buffer is not
            // needed anymore the runtime ir responsible to call the module for
            // destroying it
            OutputArgs.OutputSize = OutputString.size();
            OutputArgs.pOutput = pBufResult;
          }
          else
          {
            assert(0 && "Unrecognized output format when processing ELF input");
            success = false;
          }
        }
        else
        {
          success = false;
        }

        if (success == true)
        {
          // if -dump-opt-llvm is enabled dump the llvm output to the file
          std::string options = "";
          if((InputArgs.pOptions != nullptr) && (InputArgs.OptionsSize > 0)){
              options.append(InputArgs.pOptions, InputArgs.pOptions + InputArgs.OptionsSize);
          }
          size_t dumpOptPosition = options.find("-dump-opt-llvm");
          if (dumpOptPosition != std::string::npos)
          {
            std::string dumpFileName;
            std::istringstream iss(options.substr(dumpOptPosition));
            iss >> dumpFileName;
            size_t equalSignPosition = dumpFileName.find('=');
            if (equalSignPosition != std::string::npos)
            {
              dumpFileName = dumpFileName.substr(equalSignPosition + 1);
              // dump the buffer
              FILE* file = fopen(dumpFileName.c_str(), "wb");
              if (file != NULL)
              {
                fwrite(pBufResult, OutputString.size(), 1, file);
                fclose(file);
              }
            }
            else
            {
              std::string errorString = "\nWarning: File name not specified with the -dump-opt-llvm option.\n";
              SetErrorMessage(errorString, OutputArgs);
            }
          }
        }
      }
    }

    success = true;

  return success;
}

bool ParseInput(
    llvm::Module*& pKernelModule,
    const STB_TranslateInputArgs* pInputArgs,
    STB_TranslateOutputArgs* pOutputArgs,
    llvm::LLVMContext &oclContext,
    TB_DATA_FORMAT inputDataFormatTemp)
{
    pKernelModule = nullptr;

    // Parse the module we want to compile
    llvm::SMDiagnostic err;
    // For text IR, we don't need the null terminator
    unsigned int inputSize = pInputArgs->InputSize;

    if (inputDataFormatTemp == TB_DATA_FORMAT_LLVM_TEXT)
    {
        inputSize = strlen(pInputArgs->pInput);
    }

    llvm::StringRef strInput = llvm::StringRef(pInputArgs->pInput, inputSize);

    // IGC does not handle legacy ocl binary for now (legacy ocl binary
    // is the binary that contains text LLVM IR (2.7 or 3.0).
    if (inputSize > 1 && !(pInputArgs->pInput[0] == 'B' && pInputArgs->pInput[1] == 'C'))
    {
        bool isLLVM27IR = false, isLLVM30IR = false;

        if (strInput.find("triple = \"GHAL3D") != llvm::StringRef::npos)
        {
            isLLVM27IR = true;
        }
        else if ((strInput.find("triple = \"IGIL") != llvm::StringRef::npos) ||
            (strInput.find("metadata !\"image_access_qualifier\"") != llvm::StringRef::npos))
        {
            isLLVM30IR = true;
        }

        if (isLLVM27IR || isLLVM30IR)
        {
            SetErrorMessage("Old LLVM IR (possibly from legacy binary) :  not supported!", *pOutputArgs);
            return false;
        }
    }
  
    // BEGIN HACK
    // Upgrade BC to LLVM 3.5.1+ from LLVM 3.4+
    if (inputDataFormatTemp == TB_DATA_FORMAT_LLVM_BINARY) {
        std::unique_ptr<llvm::MemoryBuffer> Buf =
            llvm::MemoryBuffer::getMemBuffer(strInput, "<origin>", false);
        llvm::Expected<std::unique_ptr<llvm::Module>> MOE =
            upgrader::upgradeAndParseBitcodeFile(Buf->getMemBufferRef(), oclContext);
        if (llvm::Error E = MOE.takeError())
        {
            llvm::handleAllErrors(std::move(E), [&](llvm::ErrorInfoBase &EIB) {
              err = llvm::SMDiagnostic(Buf->getBufferIdentifier(), llvm::SourceMgr::DK_Error,
                                 EIB.message());
            });
        }
        else
        {
            // the MemoryBuffer becomes owned by the module and does not need to be managed
            pKernelModule = MOE->release();
        }
    }
    // END HACK
    else if (inputDataFormatTemp == TB_DATA_FORMAT_SPIR_V) {
#if defined(IGC_SPIRV_ENABLED)
        //convert SPIR-V binary to LLVM module
        std::istringstream IS(strInput);        
        std::string stringErrMsg;
        llvm::StringRef options;
        if(pInputArgs->OptionsSize > 0){
            options = llvm::StringRef(pInputArgs->pOptions, pInputArgs->OptionsSize);
        }
        std::unordered_map<uint32_t, uint64_t> specIDToSpecValueMap = UnpackSpecConstants(
                                                                            pInputArgs->pSpecConstantsIds,
                                                                            pInputArgs->pSpecConstantsValues,
                                                                            pInputArgs->SpecConstantsSize);
        bool success = spv::ReadSPIRV(oclContext, IS, pKernelModule, options, stringErrMsg, &specIDToSpecValueMap);
#else
        std::string stringErrMsg{"SPIRV consumption not enabled for the TARGET."};
        bool success = false;
#endif
        if (!success)
        {
            assert(false && stringErrMsg.c_str());
        }
    }
    else
    {
        // the MemoryBuffer becomes owned by the module and does not need to be managed
        std::unique_ptr<llvm::MemoryBuffer> pMemBuf = llvm::MemoryBuffer::getMemBuffer(strInput, "", false);
        pKernelModule = llvm::parseIR(pMemBuf->getMemBufferRef(), err, oclContext).release();
    };
    if (pKernelModule == nullptr)
    {
        err.print(nullptr, llvm::errs(), false);
        assert(false && "Parsing module failed!");
    }
    if (pKernelModule == nullptr)
    {
        SetErrorMessage("Parsing llvm module failed!", *pOutputArgs);
        return false;
    }
        
    return true;
}

#if defined(IGC_SPIRV_ENABLED)
bool ReadSpecConstantsFromSPIRV(std::istream &IS, std::vector<std::pair<uint32_t, uint32_t>> &OutSCInfo)
{
    using namespace spv;

    std::unique_ptr<SPIRVModule> BM(SPIRVModule::createSPIRVModule());
    IS >> *BM;

    auto SPV = BM->parseSpecConstants();

    for (auto& SC : SPV)
    {
        SPIRVType *type = SC->getType();
        uint32_t spec_size = type->getBitWidth() / 8;

        if (SC->hasDecorate(DecorationSpecId))
        {
            SPIRVWord spec_id = *SC->getDecorate(DecorationSpecId).begin();
            Op OP = SC->getOpCode();
            if (OP == OpSpecConstant ||
                OP == OpSpecConstantFalse ||
                OP == OpSpecConstantTrue)
            {
                OutSCInfo.push_back(std::make_pair(spec_id, spec_size));
            }
            else
            {
                assert("Wrong instruction opcode, shouldn't be here!");
                return false;
            }
        }
    }
    return true;
}
#endif

void overrideOCLProgramBinary(OpenCLProgramContext &Ctx, char *&binaryOutput, int &binarySize)
{
    auto name = DumpName(IGC::Debug::GetShaderOutputName())
        .Hash(Ctx.hash)
        .Type(ShaderType::OPENCL_SHADER)
        .Extension("progbin");

    std::string Path = name.overridePath();

    std::ifstream f(Path, std::ios::binary);
    if (!f.is_open())
        return;

    appendToShaderOverrideLogFile(Path, "OVERRIDDEN: ");

    f.seekg(0, f.end);
    int newBinarySize = (int)f.tellg();
    f.seekg(0, f.beg);

    char *newBinaryOutput = new char[newBinarySize];
    f.read(newBinaryOutput, newBinarySize);

    assert(f && "Not fully read!");

    delete[] binaryOutput;
    binaryOutput = newBinaryOutput;
    binarySize = newBinarySize;
}

void dumpOCLProgramBinary(OpenCLProgramContext &Ctx, char *binaryOutput, int binarySize)
{
#if LLVM_VERSION_MAJOR >= 7
    auto name = DumpName(IGC::Debug::GetShaderOutputName())
        .Hash(Ctx.hash)
        .Type(ShaderType::OPENCL_SHADER)
        .Extension("progbin");

    std::error_code EC;
    llvm::raw_fd_ostream f(name.str(), EC);

    if (!EC)
        f.write(binaryOutput, binarySize);
#endif
}

// Dump shader (binary or text), to default directory.
// Create directory if it doesn't exist.
// Works for all OSes.
// pExt - file name suffix (optional) and extenstion.
void DumpShaderFile(const char *pOutputFolder, const char * pBuffer, UINT bufferSize, QWORD hash, const char * pExt )
{
    using namespace std;

    stringstream ss;

    if (pBuffer && bufferSize > 0)
    {
        ss << pOutputFolder;
        ss << "OCL_"
            << "asm"
            << std::hex
            << std::setfill('0')
            << std::setw(sizeof(hash) * CHAR_BIT / 4)
            << hash
            << std::dec
            << std::setfill(' ')
            << pExt;

        FILE* pFile = NULL;
        fopen_s(&pFile, ss.str().c_str(), "wb");
        if (pFile)
        {
            fwrite(pBuffer, 1, bufferSize, pFile);
            fclose(pFile);
        }
    }
}


static bool TranslateBuildCM(const STB_TranslateInputArgs* pInputArgs,
    STB_TranslateOutputArgs* pOutputArgs,
    TB_DATA_FORMAT inputDataFormatTemp,
    const IGC::CPlatform& IGCPlatform,
    float profilingTimerResolution);

bool TranslateBuild(
    const STB_TranslateInputArgs* pInputArgs,
    STB_TranslateOutputArgs* pOutputArgs,
    TB_DATA_FORMAT inputDataFormatTemp,
    const IGC::CPlatform& IGCPlatform, 
    float profilingTimerResolution)
{
    if (pInputArgs->pInternalOptions) {
        static const char* CMC = "-cmc";
        if (strstr(pInputArgs->pInternalOptions, CMC) != nullptr)
            return TranslateBuildCM(pInputArgs,
                                    pOutputArgs,
                                    inputDataFormatTemp,
                                    IGCPlatform,
                                    profilingTimerResolution);
    }

    if (IGC_IS_FLAG_ENABLED(QualityMetricsEnable))
    {
        IGC::Debug::SetDebugFlag(IGC::Debug::DebugFlag::SHADER_QUALITY_METRICS, true);
    }
    
    MEM_USAGERESET;

    // Parse the module we want to compile
    llvm::Module* pKernelModule = nullptr;
    LLVMContextWrapper* llvmContext = new LLVMContextWrapper;
    RegisterComputeErrHandlers(*llvmContext);

    ShaderHash inputShHash = ShaderHashOCL((const UINT*)pInputArgs->pInput, pInputArgs->InputSize / 4);

    if (IGC_IS_FLAG_ENABLED(ShaderDumpEnable))
    {
        const char *pOutputFolder = IGC::Debug::GetShaderOutputFolder();
        QWORD hash = inputShHash.getAsmHash();

        if (inputDataFormatTemp == TB_DATA_FORMAT_LLVM_BINARY)
        {
            DumpShaderFile(pOutputFolder, (char *)pInputArgs->pInput, pInputArgs->InputSize, hash, ".bc");
        }
        else if (inputDataFormatTemp == TB_DATA_FORMAT_SPIR_V)
        {
            DumpShaderFile(pOutputFolder, (char *)pInputArgs->pInput, pInputArgs->InputSize, hash, ".spv");

        }

        DumpShaderFile(pOutputFolder, (char *)pInputArgs->pInternalOptions, pInputArgs->InternalOptionsSize, hash, "_internal_options.txt");
        DumpShaderFile(pOutputFolder, (char *)pInputArgs->pOptions, pInputArgs->OptionsSize, hash, "_options.txt");
    }

    if (!ParseInput(pKernelModule, pInputArgs, pOutputArgs, *llvmContext, inputDataFormatTemp))
    {
        return false;
    }
    CDriverInfoOCLNEO driverInfoOCL;
    IGC::CDriverInfo* driverInfo = &driverInfoOCL;
    
    USC::SShaderStageBTLayout zeroLayout = USC::g_cZeroShaderStageBTLayout;
    IGC::COCLBTILayout oclLayout(&zeroLayout);
    OpenCLProgramContext oclContext(oclLayout, IGCPlatform, pInputArgs, *driverInfo, llvmContext);
    COMPILER_TIME_INIT(&oclContext, m_compilerTimeStats);
    COMPILER_TIME_START(&oclContext, TIME_TOTAL);
    oclContext.m_ProfilingTimerResolution = profilingTimerResolution;

    if(inputDataFormatTemp == TB_DATA_FORMAT_SPIR_V)
    {
        oclContext.setAsSPIRV();
    }

    if (IGC_IS_FLAG_ENABLED(EnableReadGTPinInput))
    {
        // Set GTPin flags
        oclContext.gtpin_init = pInputArgs->GTPinInput;
    }

    oclContext.setModule(pKernelModule);
    if (oclContext.isSPIRV())
    {
        deserialize(*oclContext.getModuleMetaData(), pKernelModule);
    }

    oclContext.hash = inputShHash;
    oclContext.annotater = nullptr;

    // Set default denorm.
    // Note that those values have been set to FLOAT_DENORM_FLUSH_TO_ZERO
    if (IGFX_GEN8_CORE <= oclContext.platform.GetPlatformFamily())
    {
        oclContext.m_floatDenormMode16 = FLOAT_DENORM_RETAIN;
        oclContext.m_floatDenormMode32 = FLOAT_DENORM_RETAIN;
        oclContext.m_floatDenormMode64 = FLOAT_DENORM_RETAIN;
    }

    unsigned PtrSzInBits = pKernelModule->getDataLayout().getPointerSizeInBits();
    //TODO: Again, this should not happen on each compilation

    /// set retry manager
    bool retry = false;
    oclContext.m_retryManager.Enable();
    do
    {
        std::unique_ptr<llvm::Module> BuiltinGenericModule = nullptr;
        std::unique_ptr<llvm::Module> BuiltinSizeModule = nullptr;
        std::unique_ptr<llvm::MemoryBuffer> pGenericBuffer = nullptr;
        std::unique_ptr<llvm::MemoryBuffer> pSizeTBuffer = nullptr;
        {
            // IGC has two BIF Modules: 
            //            1. kernel Module (pKernelModule)
            //            2. BIF Modules:
            //                 a) generic Module (BuiltinGenericModule)
            //                 b) size Module (BuiltinSizeModule)
            //
            // OCL builtin types, such as clk_event_t/queue_t, etc., are struct (opaque) types. For
            // those types, its original names are themselves; the derived names are ones with
            // '.<digit>' appended to the original names. For example,  clk_event_t is the original
            // name, its derived names are clk_event_t.0, clk_event_t.1, etc.
            //
            // When llvm reads in multiple modules, say, M0, M1, under the same llvmcontext, if both
            // M0 and M1 has the same struct type,  M0 will have the original name and M1 the derived
            // name for that type.  For example, clk_event_t,  M0 will have clk_event_t, while M1 will
            // have clk_event_t.2 (number is arbitary). After linking, those two named types should be
            // mapped to the same type, otherwise, we could have type-mismatch (for example, OCL GAS
            // builtin_functions tests will assert during inlining due to type-mismatch).  Furthermore,
            // when linking M1 into M0 (M0 : dstModule, M1 : srcModule), the final type is the type
            // used in M0.

            // Load the builtin module -  Generic BC
            // Load the builtin module -  Generic BC
            {
                char Resource[5] = { '-' };
                _snprintf(Resource, sizeof(Resource), "#%d", OCL_BC);

                pGenericBuffer.reset(llvm::LoadBufferFromResource(Resource, "BC"));

                if (pGenericBuffer == NULL) 
                {
                    SetErrorMessage("Error loading the Generic builtin resource", *pOutputArgs);
                    return false;
                }

                llvm::Expected<std::unique_ptr<llvm::Module>> ModuleOrErr =
                    getLazyBitcodeModule(pGenericBuffer->getMemBufferRef(), *oclContext.getLLVMContext());
                
                if (llvm::Error EC = ModuleOrErr.takeError()) 
                {
                    std::string error_str = "Error lazily loading bitcode for generic builtins,"
                                            "is bitcode the right version and correctly formed?";
                    SetErrorMessage(error_str, *pOutputArgs);
                    return false;
                }
                else
                {
                    BuiltinGenericModule = std::move(*ModuleOrErr);
                }

                if (BuiltinGenericModule == NULL)
                {
                    SetErrorMessage("Error loading the Generic builtin module from buffer", *pOutputArgs);
                    return false;
                }
            }

            // Load the builtin module -  pointer depended
            {
                char ResNumber[5] = { '-' };
                switch (PtrSzInBits)
                {
                case 32:
                    _snprintf(ResNumber, sizeof(ResNumber), "#%d", OCL_BC_32);
                    break;
                case 64:
                    _snprintf(ResNumber, sizeof(ResNumber), "#%d", OCL_BC_64);
                    break;
                default:
                    assert(0 && "Unknown bitness of compiled module");
                }

                // the MemoryBuffer becomes owned by the module and does not need to be managed
                pSizeTBuffer.reset(llvm::LoadBufferFromResource(ResNumber, "BC"));
                assert(pSizeTBuffer && "Error loading builtin resource");

                llvm::Expected<std::unique_ptr<llvm::Module>> ModuleOrErr =
                    getLazyBitcodeModule(pSizeTBuffer->getMemBufferRef(), *oclContext.getLLVMContext());
                if (llvm::Error EC = ModuleOrErr.takeError())
                    assert(0 && "Error lazily loading bitcode for size_t builtins");
                else
                    BuiltinSizeModule = std::move(*ModuleOrErr);

                assert(BuiltinSizeModule
                    && "Error loading builtin module from buffer");
            }

            BuiltinGenericModule->setDataLayout(BuiltinSizeModule->getDataLayout());
            BuiltinGenericModule->setTargetTriple(BuiltinSizeModule->getTargetTriple());
        }

        oclContext.getModuleMetaData()->csInfo.forcedSIMDSize |= IGC_GET_FLAG_VALUE(ForceOCLSIMDWidth);

        if (llvm::StringRef(oclContext.getModule()->getTargetTriple()).startswith("spir"))
        {
            IGC::UnifyIRSPIR(&oclContext, std::move(BuiltinGenericModule), std::move(BuiltinSizeModule));
        }
        else // not SPIR
        {
            IGC::UnifyIROCL(&oclContext, std::move(BuiltinGenericModule), std::move(BuiltinSizeModule));
        }

        if (!(oclContext.oclErrorMessage.empty()))
        {
             //The error buffer returned will be deleted when the module is unloaded so
             //a copy is necessary
            if (const char *pErrorMsg = oclContext.oclErrorMessage.c_str())
            {
                SetErrorMessage(oclContext.oclErrorMessage, *pOutputArgs);
            }
            return false;
        }

        // Compiler Options information available after unification.
        ModuleMetaData *modMD = oclContext.getModuleMetaData();
        if (modMD->compOpt.DenormsAreZero)
        {
            oclContext.m_floatDenormMode16 = FLOAT_DENORM_FLUSH_TO_ZERO;
            oclContext.m_floatDenormMode32 = FLOAT_DENORM_FLUSH_TO_ZERO;
        }

        // Optimize the IR. This happens once for each program, not per-kernel.
        IGC::OptimizeIR(&oclContext);

        // Now, perform code generation
        IGC::CodeGen(&oclContext);

        retry = (oclContext.m_retryManager.AdvanceState() &&
                !oclContext.m_retryManager.kernelSet.empty());

        if (retry)
        {
            oclContext.clear();

            // Create a new LLVMContext
            oclContext.initLLVMContextWrapper();
            
            IGC::Debug::RegisterComputeErrHandlers(*oclContext.getLLVMContext());

            if (!ParseInput(pKernelModule, pInputArgs, pOutputArgs, *oclContext.getLLVMContext(), inputDataFormatTemp))
            {
                return false;
            }
            oclContext.setModule(pKernelModule);
        }
    } while (retry);

    // Create the binary streams for each compiled kernel
    oclContext.m_programOutput.CreateKernelBinaries();

    unsigned int pointerSizeInBytes = (PtrSzInBits == 64) ? 8 : 4; 

    // Prepare and set program binary
    Util::BinaryStream programBinary;
    oclContext.m_programOutput.GetProgramBinary(programBinary, pointerSizeInBytes);

    int binarySize = static_cast<int>(programBinary.Size());
    char* binaryOutput = new char[binarySize];
    memcpy_s(binaryOutput, binarySize, (char*)programBinary.GetLinearPointer(), binarySize);

    if (IGC_IS_FLAG_ENABLED(ShaderDumpEnable))
        dumpOCLProgramBinary(oclContext, binaryOutput, binarySize);

    if (IGC_IS_FLAG_ENABLED(ShaderOverride))
        overrideOCLProgramBinary(oclContext, binaryOutput, binarySize);

    pOutputArgs->OutputSize = binarySize;
    pOutputArgs->pOutput = binaryOutput;

    // Prepare and set program debug data
    Util::BinaryStream programDebugData;
    oclContext.m_programOutput.GetProgramDebugData(programDebugData);

    int debugDataSize = int_cast<int>(programDebugData.Size());
    if (debugDataSize > 0)
    {
        char* debugDataOutput = new char[debugDataSize];
        memcpy_s(debugDataOutput, debugDataSize, (char*)programDebugData.GetLinearPointer(), debugDataSize);

        pOutputArgs->DebugDataSize = debugDataSize;
        pOutputArgs->pDebugData = debugDataOutput;
    }

    const char* driverName =
        GTPIN_DRIVERVERSION_OPEN;
    // If GT-Pin is enabled, instrument the binary. Finally pOutputArgs will 
    // be pointing to the instrumented binary with the new size.
    if (GTPIN_IGC_OCL_IsEnabled())
    {
        const GEN_ISA_TYPE genIsa = GTPIN_IGC_OCL_GetGenIsaFromPlatform(IGCPlatform.getPlatformInfo());
        int instrumentedBinarySize = 0;
        void* instrumentedBinaryOutput = NULL;
        GTPIN_IGC_OCL_Instrument(genIsa, driverName,
            binarySize, binaryOutput,
            instrumentedBinarySize, instrumentedBinaryOutput);

        void* newBuffer = operator new[](instrumentedBinarySize, std::nothrow);
        memcpy_s(newBuffer, instrumentedBinarySize, instrumentedBinaryOutput, instrumentedBinarySize);
        pOutputArgs->OutputSize = instrumentedBinarySize;
        pOutputArgs->pOutput = (char*)newBuffer;

        if (binaryOutput != nullptr)
        {
            delete[] binaryOutput;
        }
    }

    COMPILER_TIME_END(&oclContext, TIME_TOTAL);

    COMPILER_TIME_PRINT(&oclContext, ShaderType::OPENCL_SHADER, oclContext.hash);

    COMPILER_TIME_DEL(&oclContext, m_compilerTimeStats);

    return true;
}

bool CIGCTranslationBlock::FreeAllocations(
    STB_TranslateOutputArgs* pOutputArgs)
{
    delete [] pOutputArgs->pOutput;
    return true;
}

bool CIGCTranslationBlock::Initialize(
    const STB_CreateArgs* pCreateArgs)
{
    const SGlobalData* pCreateArgsGlobalData =
                  (const SGlobalData*)pCreateArgs->pCreateData;

    // IGC maintains its own WA table - ignore the version in the global arguments.
    m_Platform = *pCreateArgsGlobalData->pPlatform;
    m_SkuTable = *pCreateArgsGlobalData->pSkuTable;
    m_SysInfo  = *pCreateArgsGlobalData->pSysInfo;

    m_DataFormatInput  = pCreateArgs->TranslationCode.Type.Input;
    m_DataFormatOutput = pCreateArgs->TranslationCode.Type.Output;

    m_ProfilingTimerResolution = pCreateArgsGlobalData->ProfilingTimerResolution;

    bool validTBChain = false;

    auto isDeviceBinaryFormat = [] (TB_DATA_FORMAT format)
    {
        return (format == TB_DATA_FORMAT_DEVICE_BINARY)
            || (format == TB_DATA_FORMAT_COHERENT_DEVICE_BINARY)
            || (format == TB_DATA_FORMAT_NON_COHERENT_DEVICE_BINARY);
    };

    validTBChain |= 
        (m_DataFormatInput == TB_DATA_FORMAT_ELF) &&
        (m_DataFormatOutput == TB_DATA_FORMAT_LLVM_BINARY);
        
    validTBChain |= 
        (m_DataFormatInput == TB_DATA_FORMAT_LLVM_TEXT) &&
        isDeviceBinaryFormat(m_DataFormatOutput);
    
    validTBChain |= 
        (m_DataFormatInput == TB_DATA_FORMAT_LLVM_BINARY) &&
        isDeviceBinaryFormat(m_DataFormatOutput);
    
    validTBChain |=
        (m_DataFormatInput == TB_DATA_FORMAT_SPIR_V) &&
        isDeviceBinaryFormat(m_DataFormatOutput);

    assert(validTBChain && "Invalid TB Chain");

    return validTBChain;
}

static const STB_TranslationCode g_cICBETranslationCodes[] =
{
    { { TB_DATA_FORMAT_ELF,           TB_DATA_FORMAT_LLVM_BINARY   } },
    { { TB_DATA_FORMAT_LLVM_TEXT,     TB_DATA_FORMAT_DEVICE_BINARY } },
    { { TB_DATA_FORMAT_LLVM_BINARY,   TB_DATA_FORMAT_DEVICE_BINARY } },
    { { TB_DATA_FORMAT_SPIR_V,        TB_DATA_FORMAT_DEVICE_BINARY } },

    { { TB_DATA_FORMAT_LLVM_TEXT,     TB_DATA_FORMAT_COHERENT_DEVICE_BINARY } },
    { { TB_DATA_FORMAT_LLVM_BINARY,   TB_DATA_FORMAT_COHERENT_DEVICE_BINARY } },
    { { TB_DATA_FORMAT_SPIR_V,        TB_DATA_FORMAT_COHERENT_DEVICE_BINARY } },
    { { TB_DATA_FORMAT_LLVM_TEXT,     TB_DATA_FORMAT_NON_COHERENT_DEVICE_BINARY } },
    { { TB_DATA_FORMAT_LLVM_BINARY,   TB_DATA_FORMAT_NON_COHERENT_DEVICE_BINARY } },
    { { TB_DATA_FORMAT_SPIR_V,        TB_DATA_FORMAT_NON_COHERENT_DEVICE_BINARY } }
};

TRANSLATION_BLOCK_API void Register(
    STB_RegisterArgs* pRegisterArgs)
{
    pRegisterArgs->Version = TC::STB_VERSION;

    if(pRegisterArgs->pTranslationCodes == NULL)
    {
        pRegisterArgs->NumTranslationCodes = 
            sizeof(g_cICBETranslationCodes ) /
            sizeof(g_cICBETranslationCodes[0]);
    }
    else
    {
        pRegisterArgs->NumTranslationCodes =
            sizeof(g_cICBETranslationCodes) /
            sizeof(g_cICBETranslationCodes[0]);

        iSTD::MemCopy<sizeof(g_cICBETranslationCodes)>(
            pRegisterArgs->pTranslationCodes,
            g_cICBETranslationCodes);
    }
}

TRANSLATION_BLOCK_API CTranslationBlock* Create( 
    STB_CreateArgs* pCreateArgs)
{
    CIGCTranslationBlock*  pIGCTranslationBlock = nullptr;

    CIGCTranslationBlock::Create(
        pCreateArgs,
        pIGCTranslationBlock);

    return pIGCTranslationBlock;
}

TRANSLATION_BLOCK_API void Delete(
    CTranslationBlock* pTranslationBlock)
{
    CIGCTranslationBlock*  pIGCTranslationBlock =
        (CIGCTranslationBlock*)pTranslationBlock;

    CIGCTranslationBlock::Delete(pIGCTranslationBlock);
}

// CMC compilation implementation.
// Convert an opaque pointer to a function pointer.
template <typename func_ptr_type>
inline func_ptr_type getFunctionType(void* ptr)
{
    intptr_t val = reinterpret_cast<intptr_t>(ptr);
    return reinterpret_cast<func_ptr_type>(val);
}

// Generate compile options.
static std::string getCommandLine(const STB_TranslateInputArgs* pInputArgs,
    TB_DATA_FORMAT inputDataFormatTemp,
    const IGC::CPlatform& IGCPlatform)
{
    std::string cmd;

    // Set the input file type.
    if (inputDataFormatTemp == TB_DATA_FORMAT::TB_DATA_FORMAT_SPIR_V)
        cmd.append(" -filetype=spv");
    else
        llvm_unreachable("not implemented yet");

    // Set the HW platform
    switch (IGCPlatform.GetPlatformFamily()) {
    default:
        llvm_unreachable("not implemented yet");
        break;
    case GFXCORE_FAMILY::IGFX_GEN9_CORE:
        cmd.append(" -platform=GEN9");
        break;
    case GFXCORE_FAMILY::IGFX_GEN10_CORE:
        cmd.append(" -platform=GEN10");
        break;
    case GFXCORE_FAMILY::IGFX_GEN10LP_CORE:
        cmd.append(" -platform=GEN10LP");
        break;
    }
    return move(cmd);
}

#if defined(_WIN64)
#define CMC_LIBRARY_NAME "igcmc64.dll"
#elif defined(_WIN32)
#define CMC_LIBRARY_NAME "igcmc32.dll"
#else
#define CMC_LIBRARY_NAME "libigcmc.so"
#endif

// Utility class to load and compile a CMC program.
struct CMCLibraryLoader {
    using compileFnTy = std::add_pointer<decltype(cmc_load_and_compile)>::type;
    using getErrorFnTy = std::add_pointer<decltype(cmc_get_error_string)>::type;
    using freeJitInfoFnTy = std::add_pointer<decltype(cmc_free_jit_info)>::type;

    using DL = llvm::sys::DynamicLibrary;
    DL Dylib;
    std::string ErrMsg;

    compileFnTy compileFn = nullptr;
    getErrorFnTy getErrorFn = nullptr;
    freeJitInfoFnTy freeJitInfoFn = nullptr;

    CMCLibraryLoader()
    {
        Dylib = DL::getPermanentLibrary(CMC_LIBRARY_NAME, &ErrMsg);
        if (Dylib.isValid()) {
            compileFn = getFunctionType<compileFnTy>(Dylib.getAddressOfSymbol("cmc_load_and_compile"));
            getErrorFn = getFunctionType<getErrorFnTy>(Dylib.getAddressOfSymbol("cmc_get_error_string"));
            freeJitInfoFn = getFunctionType<freeJitInfoFnTy>(Dylib.getAddressOfSymbol("cmc_free_jit_info"));
        }
    }

    bool isValid()
    {
        if (!Dylib.isValid())
            return false;

        if (!compileFn) {
            ErrMsg = "cannot load symbol cmc_load_and_compile";
            return false;
        }
        if (!getErrorFn) {
            ErrMsg = "cannot load symbol cmc_get_error_string";
            return false;
        }
        if (!freeJitInfoFn) {
            ErrMsg = "cannot load symbol cmc_free_jit_info";
            return false;
        }
        return true;
    }
};

// When an internal otion "-cmc" is present, compile the input as a CM program.
static bool TranslateBuildCM(const STB_TranslateInputArgs* pInputArgs,
    STB_TranslateOutputArgs* pOutputArgs,
    TB_DATA_FORMAT inputDataFormatTemp,
    const IGC::CPlatform& IGCPlatform,
    float profilingTimerResolution)
{
    CMCLibraryLoader Loader;
    if (!Loader.isValid()) {
        SetErrorMessage(Loader.ErrMsg, *pOutputArgs);
        return false;
    }

    cmc_jit_info* output = nullptr;
    const std::string cmd = getCommandLine(pInputArgs, inputDataFormatTemp, IGCPlatform);
    cmc_error_t status = Loader.compileFn(pInputArgs->pInput, pInputArgs->InputSize, cmd.c_str(), &output);
    if (status == cmc_error_t::CMC_SUCCESS) {
        // TODO: We need to refactor binary packing code.
        // Right now, the output is just Gen ASM, which should
        // be replaced by an ELF binary file with patch tokens.
        if (output) {
            size_t ByteSize = output->binary_size;
            char* Bin = new char[ByteSize];
            memcpy_s(Bin, ByteSize, (char*)output->binary, ByteSize);
            pOutputArgs->OutputSize = ByteSize;
            pOutputArgs->pOutput = Bin;

            // Free the resource allocated in the dll side.
            Loader.freeJitInfoFn(output);
        }
        return true;
    }

    // Set the error message.
    const char* Err = Loader.getErrorFn(status);
    SetErrorMessage(Err, *pOutputArgs);
    return false;
}

} // namespace TC
