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
/*
* ISA Byte Code Reader
*
* This library is designed to be extremely reusable and general in nature, and as a result
* the following byte code reader code primarily uses the following IR and data types:
*
* - common_isa_header
* - kernel_format_t
* - attribute_info_t
* - CISA_opnd
* - vector_opnd
* - raw_opnd
* - CISA_INST
* - std::list<CISA_INST*>
* - primitives (please use stdint.h types)
*
* which are used to load the byte code from a buffer into a simple structured IR.
*
* Use of any other data types should be discussed by several members of the CM jitter team before hand.
*
*/

#include <list>

#include "JitterDataStruct.h"
#include "visa_igc_common_header.h"
#include "common.h"
#include "Mem_Manager.h"
#include "Common_ISA.h"
#include "Common_ISA_framework.h"
#include "Common_ISA_util.h"
#include "VISADefines.h"
#include "IsaDisassembly.h"

#include "VISAKernel.h"
#include "BuildCISAIR.h"

#include "Gen4_IR.hpp"
#include "BuildIR.h"

using namespace std;

struct RoutineContainer
{
    RoutineContainer():
        generalVarDecls(NULL),   generalVarsCount(0),
        addressVarDecls(NULL),   addressVarsCount(0),
        predicateVarDecls(NULL), predicateVarsCount(0),
        samplerVarDecls(NULL),   samplerVarsCount(0),
        surfaceVarDecls(NULL),   surfaceVarsCount(0),
        labelVarDecls(NULL),     labelVarsCount(0),
        inputVarDecls(NULL),     inputVarsCount(0),
        majorVersion(0),
        minorVersion(0) { }

    ~RoutineContainer()
    {
        stringPool.clear();
    }
    VISA_GenVar**       generalVarDecls; unsigned   generalVarsCount;
    VISA_AddrVar**      addressVarDecls; unsigned   addressVarsCount;
    VISA_PredVar**    predicateVarDecls; unsigned predicateVarsCount;
    VISA_SamplerVar**   samplerVarDecls; unsigned   samplerVarsCount;
    VISA_SurfaceVar**   surfaceVarDecls; unsigned   surfaceVarsCount;
    VISA_LabelOpnd**      labelVarDecls; unsigned     labelVarsCount;
    CISA_GEN_VAR**        inputVarDecls; unsigned     inputVarsCount;

    vector<string> stringPool;

    CISA_IR_Builder* builder = nullptr;
    VISAKernel*      kernelBuilder = nullptr;
    uint8_t majorVersion;
    uint8_t minorVersion;

};

/// Assumming buf is start of the CISA byte code.
#define GET_MAJOR_VERSION(buf) (*((unsigned char*) &buf[4]))
#define GET_MINOR_VERSION(buf) (*((unsigned char*) &buf[5]))

#define READ_CISA_FIELD(dst, type, bytePos, buf) \
    do {                                             \
    dst = *((type *) &buf[bytePos]);             \
    bytePos += sizeof(type);                     \
    } while (0)

#define PEAK_CISA_FIELD(dst, type, bytePos, buf) \
    do {                                             \
    dst = *((type *) &buf[bytePos]);             \
    } while (0)

typedef enum {
    CISA_EMASK_M0,
    CISA_EMASK_M1,
    CISA_EMASK_M2,
    CISA_EMASK_M3,
    CISA_EMASK_M4,
    CISA_EMASK_M5,
    CISA_EMASK_M6,
    CISA_EMASK_M7,
    CISA_NO_EMASK,
    CISA_DEF_EMASK
} Common_ISA_EMask_Ctrl_3_0;

enum class FIELD_TYPE
{
    DECL,
    INPUT
};

// vISA 3.4+ supports 32-bit general variable IDs
// vISA 3.5+ supports 32-bit input count
template <typename T>
inline void readVarBytes(uint8_t major, uint8_t minor, T& dst, uint32_t& bytePos, const char* buf, FIELD_TYPE field = FIELD_TYPE::DECL)
{
    static_assert(std::is_integral<T>::value && (sizeof(T) == 2 || sizeof(T) == 4), "T should be short or int");
    uint32_t version = getVersionAsInt(major, minor);
    uintptr_t ptrval = reinterpret_cast<uintptr_t>(&buf[bytePos]);
    bool get4Bytes = false;
    if (field == FIELD_TYPE::DECL)
    {
        get4Bytes = (version >= getVersionAsInt(3, 4));
    }
    else if (field == FIELD_TYPE::INPUT)
    {
        get4Bytes = (version >= getVersionAsInt(3, 5));
    }

    if (get4Bytes)
    {
        dst = *(reinterpret_cast<uint32_t*>(ptrval));
        bytePos += sizeof(uint32_t);
    }
    else if (field == FIELD_TYPE::INPUT)
    {
        dst = *(reinterpret_cast<uint8_t*>(ptrval));
        bytePos += sizeof(uint8_t);
    }
    else
    {
        dst = *(reinterpret_cast<uint16_t*>(ptrval));
        bytePos += sizeof(uint16_t);
    }
}

static Common_VISA_EMask_Ctrl transformMask(
    RoutineContainer& container, uint8_t maskVal)
{
    Common_VISA_EMask_Ctrl mask = vISA_EMASK_M1;
    if (container.majorVersion == 3 && container.minorVersion == 0)
    {
        Common_ISA_EMask_Ctrl_3_0 tMask = Common_ISA_EMask_Ctrl_3_0(maskVal);
        switch (tMask)
        {
        case CISA_EMASK_M0:
            {
                mask = vISA_EMASK_M1;
                break;
            }
        case CISA_EMASK_M1:
            {
                mask = vISA_EMASK_M2;
                break;
            }
        case CISA_EMASK_M2:
            {
                mask = vISA_EMASK_M3;
                break;
            }
        case CISA_EMASK_M3:
            {
                mask = vISA_EMASK_M4;
                break;
            }
        case CISA_EMASK_M4:
            {
                mask = vISA_EMASK_M5;
                break;
            }
        case CISA_EMASK_M5:
            {
                mask = vISA_EMASK_M6;
                break;
            }
        case CISA_EMASK_M6:
            {
                mask = vISA_EMASK_M7;
                break;
            }
        case CISA_EMASK_M7:
            {
                mask = vISA_EMASK_M8;
                break;
            }
        case CISA_NO_EMASK:
            {
                mask = vISA_EMASK_M1_NM;
                break;
            }
        case CISA_DEF_EMASK:
            {
                mask = vISA_EMASK_M1;
                break;
            }
        default:
            break;
        }
    }else
    {
        mask = Common_VISA_EMask_Ctrl(maskVal);
    }

    return mask;
}
static void readExecSizeNG(unsigned& bytePos, const char* buf, Common_ISA_Exec_Size& size, Common_VISA_EMask_Ctrl& mask, RoutineContainer& container)
{
    uint8_t execSize = 0;
    READ_CISA_FIELD(execSize, uint8_t, bytePos, buf);
    uint8_t maskVal = (execSize >> 0x4) & 0xF;

    mask = transformMask(container, maskVal);

    size = (Common_ISA_Exec_Size )((execSize) & 0xF);
}

template <typename T> T readPrimitiveOperandNG(unsigned& bytePos, const char* buf)
{
    MUST_BE_TRUE(buf, "Argument Exception: argument buf  is NULL.");
    T data = 0;
    READ_CISA_FIELD(data, T, bytePos, buf);
    return data;
}

static VISA_PredOpnd* readPredicateOperandNG(unsigned& bytePos, const char* buf, RoutineContainer& container)
{
    uint16_t predOpnd = 0;
    READ_CISA_FIELD(predOpnd, uint16_t, bytePos, buf);

    if (0 == predOpnd) return NULL;

    VISAKernel* kernelBuilder = container.kernelBuilder;
    unsigned predID = (predOpnd & 0xfff);
    VISA_PREDICATE_CONTROL control = (VISA_PREDICATE_CONTROL)((predOpnd & 0x6000) >> 13);
    VISA_PREDICATE_STATE   state   = (VISA_PREDICATE_STATE  )((predOpnd & 0x8000) >> 15);
    VISA_PredVar*  decl = container.predicateVarDecls[predID];
    VISA_PredOpnd* opnd = NULL;

    kernelBuilder->CreateVISAPredicateOperand(opnd, decl, state, control);

    return opnd;
}

static VISA_RawOpnd* readRawOperandNG(unsigned& bytePos, const char* buf, RoutineContainer& container)
{
    MUST_BE_TRUE(buf, "Argument Exception: argument buf  is NULL.");
    uint8_t majorVersion = container.majorVersion;
    uint8_t minorVersion = container.minorVersion;

    uint32_t index  = 0;
    uint16_t offset = 0;
    readVarBytes(majorVersion, minorVersion, index, bytePos, buf);
    READ_CISA_FIELD(offset, uint16_t, bytePos, buf);

    VISAKernelImpl* kernelBuilderImpl = ((VISAKernelImpl*)container.kernelBuilder);


    unsigned numPreDefinedVars = Get_CISA_PreDefined_Var_Count();
    VISA_GenVar* decl = NULL;
    VISA_RawOpnd* opnd = NULL;

    /**
        Null register is treated differently now. There is special operand NullReg created.
        In it field isNullReg is set to true.

        TODO:? To make things more generic need to mark decl created during initialization as
        null register, then when CreateVisaRawOperand is called check that decl passed in is
        null register decl, and mark operand is nullReg, also create region <0;1,0>
    */
    if(index == 0)
    {
       kernelBuilderImpl->CreateVISANullRawOperand(opnd, true); //dst
    }
    else
    {
        if (index >= numPreDefinedVars)
            decl = container.generalVarDecls[index];
        else
            kernelBuilderImpl->GetPredefinedVar(decl, (PreDefined_Vars)index);

        kernelBuilderImpl->CreateVISARawOperand(opnd, decl, offset);
    }

    return opnd;
}

static VISA_PredVar* readPreVarNG(unsigned& bytePos, const char* buf, RoutineContainer& container)
{
    MUST_BE_TRUE(buf, "Argument Exception: argument buf  is NULL.");

    uint8_t tag = 0;
    READ_CISA_FIELD(tag, uint8_t, bytePos, buf);

    uint16_t index = 0;
    READ_CISA_FIELD(index, uint16_t, bytePos, buf);

    uint16_t predIndex = index & 0xfff;
    VISA_PredVar*  decl = NULL;

    if (predIndex >= COMMON_ISA_NUM_PREDEFINED_PRED)
        decl = container.predicateVarDecls[predIndex];
    return decl;
}

static uint32_t readOtherOperandNG(unsigned& bytePos, const char* buf, VISA_Type visatype)
{
    union {
        uint32_t other_opnd;
        struct {
            uint8_t b[4];
        };
    } v;

    unsigned bsize = CISATypeTable[visatype].typeSize;
    assert(bsize <= 4 && " Unsupported other_opnd whose size > 4 bytes!");
    v.other_opnd = 0;
    for (int i = 0; i < (int)bsize; ++i)
    {
        v.b[i] = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
    }
    return v.other_opnd;
}

static VISA_VectorOpnd* readVectorOperandNG(unsigned& bytePos, const char* buf, uint8_t& tag, RoutineContainer& container, unsigned int size, bool isDst, bool isAddressoff = false)
{
    MUST_BE_TRUE(buf, "Argument Exception: argument buf  is NULL.");

    VISAKernelImpl* kernelBuilderImpl = ((VISAKernelImpl*)container.kernelBuilder);

    uint8_t majorVersion = container.majorVersion;
    uint8_t minorVersion = container.minorVersion;

    READ_CISA_FIELD(tag, uint8_t, bytePos, buf);
    VISA_Modifier modifier = ((VISA_Modifier)((tag >> 3 ) & 0x7));

    switch ((Common_ISA_Operand_Class)(tag & 0x7)) /// getOperandClass
    {
    case OPERAND_GENERAL:
        {
            uint32_t index     = 0;
            uint8_t  rowOffset = 0;
            uint8_t  colOffset = 0;
            uint16_t region    = 0;

            readVarBytes(majorVersion, minorVersion, index, bytePos, buf);
            READ_CISA_FIELD(rowOffset , uint8_t , bytePos, buf);
            READ_CISA_FIELD(colOffset , uint8_t , bytePos, buf);
            READ_CISA_FIELD(region    , uint16_t, bytePos, buf);

            uint16_t v_stride = Get_Common_ISA_Region_Value((Common_ISA_Region_Val)(region & 0xF));
            uint16_t width    = Get_Common_ISA_Region_Value((Common_ISA_Region_Val)((region >> 4 ) & 0xF));
            uint16_t h_stride = Get_Common_ISA_Region_Value((Common_ISA_Region_Val)((region >> 8 ) & 0xF));

            unsigned numPreDefinedVars = Get_CISA_PreDefined_Var_Count();

            VISA_Modifier     mod = modifier;
            VISA_VectorOpnd* opnd = NULL;
            VISA_GenVar*     decl = NULL;

            if (index >= numPreDefinedVars)
                decl = container.generalVarDecls[index];
            else
                kernelBuilderImpl->GetPredefinedVar(decl, (PreDefined_Vars)index);

            if (isDst)
                kernelBuilderImpl->CreateVISADstOperand(opnd, decl, h_stride, rowOffset, colOffset);
            else if(isAddressoff)
            {
                VISA_Type vType = decl->genVar.getType();
                G4_Type gType = Get_G4_Type_From_Common_ISA_Type(vType);
                unsigned int offset  = colOffset * G4_Type_Table[gType].byteSize + rowOffset * G4_GRF_REG_NBYTES;
                kernelBuilderImpl->CreateVISAAddressOfOperand(opnd, decl, offset);
            }
            else
            {
                kernelBuilderImpl->CreateVISASrcOperand(opnd, decl, mod, v_stride, width, h_stride, rowOffset, colOffset);
            }

            return opnd;
        }
    case OPERAND_ADDRESS:
        {
            uint16_t index  = 0;
            uint8_t  offset = 0;
            uint16_t width  = 0;

            READ_CISA_FIELD(index , uint16_t, bytePos, buf);
            READ_CISA_FIELD(offset, uint8_t , bytePos, buf);
            READ_CISA_FIELD(width , uint8_t , bytePos, buf);

            VISA_VectorOpnd* opnd = NULL;
            VISA_AddrVar*    decl = container.addressVarDecls[index];
            kernelBuilderImpl->CreateVISAAddressOperand(opnd, decl, offset, Get_Common_ISA_Exec_Size((Common_ISA_Exec_Size)width), isDst);

            return opnd;
        }
    case OPERAND_PREDICATE:
        {
            uint16_t index = 0;
            READ_CISA_FIELD(index, uint16_t, bytePos, buf);

            uint16_t predIndex = index & 0xfff;
            VISA_PredVar*  decl = NULL;

            if (predIndex >= COMMON_ISA_NUM_PREDEFINED_PRED)
                decl = container.predicateVarDecls[predIndex];

            VISA_VectorOpnd* opnd = nullptr;
            if (isDst)
            {
                kernelBuilderImpl->CreateVISAPredicateDstOperand(opnd, decl, size);
                return opnd;
            }
            else
            {
                kernelBuilderImpl->CreateVISAPredicateSrcOperand(opnd, decl, size);
                return opnd;
            }
        }
    case OPERAND_INDIRECT:
        {
            uint16_t index           = 0;
            uint8_t  addr_offset     = 0;
            int16_t  indirect_offset = 0;
            uint8_t  bit_property    = 0;
            uint16_t region          = 0;

            READ_CISA_FIELD(index          , uint16_t, bytePos, buf);
            READ_CISA_FIELD(addr_offset    , uint8_t , bytePos, buf);
            READ_CISA_FIELD(indirect_offset, int16_t , bytePos, buf);
            READ_CISA_FIELD(bit_property   , uint8_t , bytePos, buf);
            READ_CISA_FIELD(region         , uint16_t, bytePos, buf);

            uint16_t v_stride = Get_Common_ISA_Region_Value((Common_ISA_Region_Val)((region      ) & 0xF));
            uint16_t width    = Get_Common_ISA_Region_Value((Common_ISA_Region_Val)((region >> 4 ) & 0xF));
            uint16_t h_stride = Get_Common_ISA_Region_Value((Common_ISA_Region_Val)((region >> 8 ) & 0xF));

            VISA_Modifier       mod = modifier;
            VISA_VectorOpnd*   opnd = NULL;
            VISA_AddrVar*      decl = container.addressVarDecls[index];

            kernelBuilderImpl->CreateVISAIndirectGeneralOperand(
                opnd, decl, mod, addr_offset, indirect_offset, v_stride, width, h_stride,
                (VISA_Type)(bit_property & 0xF), isDst);

            return opnd;
        }
    case OPERAND_IMMEDIATE:
        {
            uint8_t type = 0;
            READ_CISA_FIELD(type, uint8_t, bytePos, buf);
            VISA_Type immedType = (VISA_Type)(type & 0xF);

            VISA_VectorOpnd* opnd = NULL;

            if (immedType == ISA_TYPE_DF)
            {
                double val = 0;
                READ_CISA_FIELD(val, double, bytePos, buf);
                kernelBuilderImpl->CreateVISAImmediate(opnd, &val, immedType);
            }
            else if (immedType == ISA_TYPE_Q || immedType == ISA_TYPE_UQ)
            {
                uint64_t val = 0;
                READ_CISA_FIELD(val, uint64_t, bytePos, buf);
                kernelBuilderImpl->CreateVISAImmediate(opnd, &val, immedType);
            }
            else /// Immediate operands are at least 4 bytes.
            {
                unsigned val = 0;
                READ_CISA_FIELD(val, unsigned, bytePos, buf);
                kernelBuilderImpl->CreateVISAImmediate(opnd, &val, immedType);

            }

            return opnd;
        }
    case OPERAND_STATE:
        {
            uint8_t  opnd_class = 0;
            uint16_t index      = 0;
            uint8_t  offset     = 0;

            READ_CISA_FIELD(opnd_class, uint8_t , bytePos, buf);
            READ_CISA_FIELD(index     , uint16_t, bytePos, buf);
            READ_CISA_FIELD(offset    , uint8_t , bytePos, buf);

            VISA_VectorOpnd* opnd = NULL;

            switch ((Common_ISA_State_Opnd_Class)opnd_class)
            {
            case STATE_OPND_SURFACE:
                {
                    if(isAddressoff)
                    {
                        VISA_SurfaceVar* decl = container.surfaceVarDecls[index];
                        unsigned int offsetB = offset * G4_Type_Table[Type_UW].byteSize;
                        kernelBuilderImpl->CreateVISAAddressOfOperand(opnd, decl, offsetB);
                    }else
                    {
                        VISA_SurfaceVar* decl = container.surfaceVarDecls[index];
                        kernelBuilderImpl->CreateVISAStateOperand(opnd, decl, (uint8_t) size, offset, isDst);
                    }
                    break;
                }
            case STATE_OPND_SAMPLER:
                {
                    VISA_SamplerVar* decl = container.samplerVarDecls[index];
                    if (isAddressoff)
                    {
                        unsigned int offsetB = offset * G4_Type_Table[Type_UW].byteSize;
                        kernelBuilderImpl->CreateVISAAddressOfOperandGeneric(opnd, decl, offsetB);
                    }
                    else
                    {
                        kernelBuilderImpl->CreateVISAStateOperand(opnd, decl, (uint8_t)size, offset, isDst);
                    }
                    break;
                }
            default:
                {
                    MUST_BE_TRUE(false, "Invalid state operand class: only surface and sampler are supported.");
                    break;
                }
            }

            return opnd;
        }
    default:
        MUST_BE_TRUE(false, "Operand class not recognized");
        return NULL;
    }
}

static VISA_VectorOpnd* readVectorOperandNG(unsigned& bytePos, const char* buf, RoutineContainer& container, unsigned int size)
{
    uint8_t tag = 0;
    bool isDst = false;
    return readVectorOperandNG(bytePos, buf, tag, container, size, isDst);
}

static VISA_VectorOpnd* readVectorOperandNG(unsigned& bytePos, const char* buf, RoutineContainer& container,  bool isDst)
{
    uint8_t tag = 0;
    return readVectorOperandNG(bytePos, buf, tag, container, 1, isDst);
}

static VISA_VectorOpnd * readVectorOperandNGAddressOf(unsigned& bytePos, const char* buf, RoutineContainer& container)
{
    uint8_t tag = 0;
    bool isDst = false;
    bool isAddressOff = true;
    return readVectorOperandNG(bytePos, buf, tag, container, 1, isDst, isAddressOff);
}

static void readInstructionCommonNG(unsigned& bytePos, const char* buf, ISA_Opcode opcode, RoutineContainer& container)
{
    Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
    Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;

    VISA_INST_Desc* inst_desc = &CISA_INST_table[opcode];
    unsigned opnd_count = inst_desc->opnd_num;
    unsigned opnd_skip = 0;

    for (unsigned i = 0; i < 2; i++)
    {
        if ((opnd_count-opnd_skip) > 0 &&
            (inst_desc->opnd_desc[i].opnd_type == OPND_EXECSIZE ||
            inst_desc->opnd_desc[i].opnd_type == OPND_PRED))
        {
            opnd_skip++;
        }
    }

    VISAKernel* kernelBuilder = container.kernelBuilder;

    switch(ISA_Inst_Table[opcode].type)
    {
    case ISA_Inst_Mov:
    case ISA_Inst_Arith:
    case ISA_Inst_Logic:
    case ISA_Inst_Address:
    case ISA_Inst_Compare:
        {
            VISA_VectorOpnd* opnds[COMMON_ISA_MAX_NUM_OPND_ARITH_LOGIC];
            ASSERT_USER(opnd_count <= COMMON_ISA_MAX_NUM_OPND_ARITH_LOGIC, "Insturction operand count exceeds maximum supported operands.");
            memset(opnds, 0, sizeof(VISA_VectorOpnd*) * COMMON_ISA_MAX_NUM_OPND_ARITH_LOGIC);

            readExecSizeNG(bytePos, buf, esize, emask, container);
            VISA_PredOpnd* pred = hasPredicate(opcode) ? readPredicateOperandNG(bytePos, buf, container) : NULL;

            uint8_t opSpec = 0;
            if (ISA_FMINMAX == opcode || ISA_CMP == opcode)
            {
                opSpec = readPrimitiveOperandNG<uint8_t>(bytePos, buf); /// rel_Op or opext
                opnd_skip++;
            }

            uint32_t exSize = Get_Common_ISA_Exec_Size(esize);
            uint8_t tag = 0;
            VISA_PredVar* dstDcl = NULL;
            bool cmpHasDst = false;
            for (unsigned i = 0; i < opnd_count-opnd_skip; i++)
            {
                bool isDst = i == 0;

                if ((OPND_DST_GEN & inst_desc->opnd_desc[i+opnd_skip].opnd_type) != 0)
                {
                    isDst = true;
                }

                if (isDst)
                {
                    if(ISA_Inst_Table[opcode].type == ISA_Inst_Compare)
                    {
                        opnds[i] = NULL;
                        PEAK_CISA_FIELD(tag, uint8_t, bytePos, buf);

                        if( (tag & 0x7) == OPERAND_GENERAL )
                        {
                            opnds[i] = readVectorOperandNG(bytePos, buf, tag, container, Get_Common_ISA_Exec_Size(esize), true);
                            cmpHasDst = true;

                        }else
                        {
                            dstDcl = readPreVarNG(bytePos, buf, container);
                        }
                    }
                    else
                        opnds[i] = readVectorOperandNG(bytePos, buf, tag, container, exSize, isDst);
                }
                else if(ISA_Inst_Table[opcode].type == ISA_Inst_Address && i == 1)
                {
                    //for first source of address add instruction.
                    opnds[i] = readVectorOperandNGAddressOf(bytePos, buf, container);
                }
                else
                {
                    opnds[i] = readVectorOperandNG(bytePos, buf, container, exSize);
                }

            }

            opnd_count -= opnd_skip;


            bool             saturate = (((VISA_Modifier)((tag >> 3 ) & 0x7)) == MODIFIER_SAT);
            VISA_VectorOpnd*      dst = opnds[0];
            VISA_VectorOpnd*     src0 = opnds[1];
            VISA_VectorOpnd*     src1 = opnd_count > 2 ? opnds[2] : NULL;
            VISA_VectorOpnd*     src2 = opnd_count > 3 ? opnds[3] : NULL;
            VISA_VectorOpnd*     src3 = opnd_count > 4 ? opnds[4] : NULL;

            switch(ISA_Inst_Table[opcode].type)
            {
            case ISA_Inst_Mov:
                if (opcode == ISA_FMINMAX)
                    kernelBuilder->AppendVISAMinMaxInst((CISA_MIN_MAX_SUB_OPCODE)opSpec, saturate, emask, esize, dst, src0, src1);
                else
                    kernelBuilder->AppendVISADataMovementInst(opcode, pred, saturate, emask, esize, dst, src0, src1);
                break;
            case ISA_Inst_Arith:
                if (opcode == ISA_ADDC || opcode == ISA_SUBB)
                {
                    kernelBuilder->AppendVISAArithmeticInst(opcode, pred, emask, esize, dst, src0, src1, src2);
                }
                else
                {
                    kernelBuilder->AppendVISAArithmeticInst(opcode, pred, saturate, emask, esize, dst, src0, src1, src2);
                }
                break;
            case ISA_Inst_Logic:
                    kernelBuilder->AppendVISALogicOrShiftInst(opcode, pred, saturate, emask, esize, dst, src0, src1, src2, src3);
                break;
            case ISA_Inst_Address:
                kernelBuilder->AppendVISAAddrAddInst(emask, esize, dst, src0, src1);
                break;
            case ISA_Inst_Compare:
                if(dstDcl)
                    kernelBuilder->AppendVISAComparisonInst((Common_ISA_Cond_Mod)(opSpec & 0x7), emask, esize, dstDcl, src0, src1);
                else if(cmpHasDst)
                    kernelBuilder->AppendVISAComparisonInst((Common_ISA_Cond_Mod)(opSpec & 0x7), emask, esize, dst, src0, src1);
                else
                    ASSERT_USER(true, "DST doesn't have valid GRF or FLAG dst.");
                break;
            default:
                break;
            }

            break;
        }
    case ISA_Inst_SIMD_Flow:
        {
            readExecSizeNG(bytePos, buf, esize, emask, container);
            VISA_PredOpnd* pred = hasPredicate(opcode) ? readPredicateOperandNG(bytePos, buf, container) : NULL;
            VISA_LabelOpnd* label = opcode == ISA_GOTO ?  container.labelVarDecls[readPrimitiveOperandNG<uint16_t>(bytePos, buf)] : NULL;
            kernelBuilder->AppendVISACFSIMDInst(opcode, pred, emask, esize, label);
            break;
        }
    case ISA_Inst_Sync:
        {
            if (opcode == ISA_WAIT )
            {
                VISA_VectorOpnd* mask = NULL;
                if (getVersionAsInt(container.majorVersion, container.minorVersion) >=
                    getVersionAsInt(3, 1))
                {
                    // additional vector operand
                    mask = readVectorOperandNG(bytePos, buf, container, false);
                }
                else
                {
                    // set mask to 0
                    uint16_t value = 0;
                    kernelBuilder->CreateVISAImmediate(mask, &value, ISA_TYPE_UW);
                }
                kernelBuilder->AppendVISAWaitInst(mask);
            }
            else if (opcode == ISA_SBARRIER)
            {
                uint32_t mode = readOtherOperandNG(bytePos, buf, ISA_TYPE_UB);
                kernelBuilder->AppendVISASplitBarrierInst(mode != 0);
            }
            else
            {
                bool hasMask = (opcode == ISA_FENCE);
                uint8_t mask = hasMask ? readPrimitiveOperandNG<uint8_t>(bytePos, buf) : 0;
                kernelBuilder->AppendVISASyncInst(opcode, mask);
            }
            break;
        }
    default:
        {
            assert(false && "Invalid common instruction type.");
        }
    }
}

/// Read a byte which encodes the atomic opcode and a flag indicating whether
/// this is a 16bit atomic operation.
std::tuple<VISAAtomicOps, unsigned short> getAtomicOpAndBitwidth(unsigned &bytePos,
                                                                 const char *buf)
{
    // bits 0-4 atomic op and bit 5-6 encode the bitwidth
    uint8_t data = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
    VISAAtomicOps op = static_cast<VISAAtomicOps>(data & 0x1F);
    unsigned short bitwidth;
    if (data >> 5 == 1)
        bitwidth = 16;
    else if (data >> 6 == 1)
        bitwidth = 64;
    else
        bitwidth = 32;
    return std::tie(op, bitwidth);
}

static void readInstructionDataportNG(unsigned& bytePos, const char* buf, ISA_Opcode opcode, RoutineContainer& container)
{
    VISAKernel*     kernelBuilder     = container.kernelBuilder;
    VISAKernelImpl* kernelBuilderImpl = ((VISAKernelImpl*)kernelBuilder);

    switch (opcode)
    {
    case ISA_MEDIA_ST:
    case ISA_MEDIA_LD:
        {
            uint8_t      modifier = (ISA_MEDIA_LD == opcode || ISA_MEDIA_ST == opcode) ? readPrimitiveOperandNG<uint8_t>(bytePos, buf) : 0;
            uint8_t       surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            uint8_t         plane = (ISA_MEDIA_LD == opcode || ISA_MEDIA_ST == opcode) ? readPrimitiveOperandNG<uint8_t>(bytePos, buf) : 0;
            uint8_t         width = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            uint8_t        height = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISA_opnd*    xoffset = readVectorOperandNG(bytePos, buf, container, false);
            VISA_opnd*    yoffset = readVectorOperandNG(bytePos, buf, container, false);
            VISA_RawOpnd*     msg = readRawOperandNG(bytePos, buf, container);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);

            kernelBuilder->AppendVISASurfAccessMediaLoadStoreInst(opcode, (MEDIA_LD_mod)modifier, surfaceHnd, width, height, (VISA_VectorOpnd*)xoffset, (VISA_VectorOpnd*)yoffset, msg, (CISA_PLANE_ID)plane);
            break;
        }
    case ISA_OWORD_ST:
    case ISA_OWORD_LD:
    case ISA_OWORD_LD_UNALIGNED:
        {
            uint8_t              size = readPrimitiveOperandNG<uint8_t>(bytePos, buf) & 0x7;
            if (ISA_OWORD_ST != opcode)
            {
                readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            }  // modifier
            uint8_t           surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISA_VectorOpnd*   offset = readVectorOperandNG(bytePos, buf, container, false);
            VISA_RawOpnd*         msg = readRawOperandNG(bytePos, buf, container);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
            kernelBuilder->AppendVISASurfAccessOwordLoadStoreInst(opcode, vISA_EMASK_M1, surfaceHnd, (Common_ISA_Oword_Num)size, (VISA_VectorOpnd*)offset, msg);

            break;
        }
    case ISA_GATHER:
    case ISA_SCATTER:
        {
            uint8_t elt_size = (ISA_SCATTER  == opcode || ISA_GATHER  == opcode) ? readPrimitiveOperandNG<uint8_t>(bytePos, buf) : 0;
            if (ISA_GATHER == opcode)
            {
                readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            } // modifier
            uint8_t num_elts = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            uint8_t surface  = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            VISA_VectorOpnd*  globalOffset = readVectorOperandNG(bytePos, buf, container, false);
            VISA_RawOpnd*    elementOffset = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd*              msg = readRawOperandNG(bytePos, buf, container);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);

            Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
            Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;

            /// TODO: Conversions like these make using vISA builder cumbersome.
            switch (num_elts & 0x3)
            {
            case 0:
                esize = EXEC_SIZE_8;
                break;
            case 1:
                esize = EXEC_SIZE_16;
                break;
            case 2:
                esize = EXEC_SIZE_1;
                break;
            default:
                MUST_BE_TRUE(false, "Invalid Number of Elements for Gather/Scatter." );
            }

            emask = transformMask(container, num_elts >> 4);

            kernelBuilderImpl->AppendVISASurfAccessGatherScatterInst(opcode, emask, (GATHER_SCATTER_ELEMENT_SIZE)(elt_size & 0x3), esize, surfaceHnd, globalOffset, elementOffset, msg);
            break;
        }
    case ISA_GATHER4_TYPED:
    case ISA_SCATTER4_TYPED:
        {
            if (getVersionAsInt(container.majorVersion, container.minorVersion) >=
                    getVersionAsInt(3, 2))
            {
                Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
                Common_ISA_Exec_Size esize = EXEC_SIZE_ILLEGAL;
                readExecSizeNG(bytePos, buf, esize, emask, container);

                VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);
                unsigned ch_mask = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
                uint8_t surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

                VISA_RawOpnd* uOffset = readRawOperandNG(bytePos, buf, container);
                VISA_RawOpnd* vOffset = readRawOperandNG(bytePos, buf, container);
                VISA_RawOpnd* rOffset = readRawOperandNG(bytePos, buf, container);
                VISA_RawOpnd* lod = readRawOperandNG(bytePos, buf, container);
                VISA_RawOpnd* msg = readRawOperandNG(bytePos, buf, container);

                VISA_StateOpndHandle* surfaceHnd = NULL;
                kernelBuilderImpl->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
                kernelBuilderImpl->AppendVISASurfAccessGather4Scatter4TypedInst(opcode, pred, ChannelMask::createAPIFromBinary(opcode, ch_mask), emask, esize, surfaceHnd, uOffset, vOffset, rOffset, lod, msg);
            }
            else
            {
                uint8_t ch_mask = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

                Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
                Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;
                readExecSizeNG(bytePos, buf, esize, emask, container);

                MUST_BE_TRUE(esize == 0, "Unsupported number of elements for ISA_SCATTER4_TYPED/ISA_GATHER4_TYPED.");
                esize = EXEC_SIZE_8;

                uint8_t surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

                VISA_RawOpnd* uOffset = readRawOperandNG(bytePos, buf, container);
                VISA_RawOpnd* vOffset = readRawOperandNG(bytePos, buf, container);
                VISA_RawOpnd* rOffset = readRawOperandNG(bytePos, buf, container);
                VISA_RawOpnd* lod = NULL;
                kernelBuilderImpl->CreateVISANullRawOperand(lod, false);
                VISA_RawOpnd*     msg = readRawOperandNG(bytePos, buf, container);

                VISA_StateOpndHandle* surfaceHnd = NULL;
                kernelBuilderImpl->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
                ch_mask = ~ch_mask;
                kernelBuilderImpl->AppendVISASurfAccessGather4Scatter4TypedInst(opcode, NULL, ChannelMask::createAPIFromBinary(opcode, ch_mask), emask, esize, surfaceHnd, uOffset, vOffset, rOffset, lod, msg);
            }
            break;
        }
    case ISA_3D_RT_WRITE:
        {
            Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
            Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;
            readExecSizeNG(bytePos, buf, esize, emask, container);

            VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);

            uint16_t    mode = readPrimitiveOperandNG<uint16_t>(bytePos, buf);
            uint8_t surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISA_RawOpnd *r1HeaderOpnd = readRawOperandNG(bytePos, buf, container);

            vISA_RT_CONTROLS cntrls;
            cntrls.s0aPresent   = (mode & (0x1 << 3))?true:false;
            cntrls.oMPresent    = (mode & (0x1 << 4))?true:false;
            cntrls.zPresent     = (mode & (0x1 << 5))?true:false;
            cntrls.isStencil    = (mode & (0x1 << 6))?true:false;
            cntrls.isLastWrite  = (mode & (0x1 << 7))?true:false;
            bool CPSEnable      = (mode & (0x1 << 8))?true:false;
            cntrls.isPerSample   = (mode & (0x1 << 9))?true:false;
            cntrls.isCoarseMode  = (mode & (0x1 << 10))? true:false;
            cntrls.isSampleIndex = (mode & (0x1 << 11)) ? true : false;
            cntrls.RTIndexPresent = (mode & (0x1 << 2)) ? true : false;
            cntrls.isHeaderMaskfromCe0 = 0;

            VISA_VectorOpnd *sampleIndex = cntrls.isSampleIndex ? readVectorOperandNG(bytePos, buf, container, false) : NULL;
            VISA_VectorOpnd *cpsCounter = CPSEnable ? readVectorOperandNG(bytePos, buf, container, false) : NULL;
            VISA_VectorOpnd* rti = cntrls.RTIndexPresent ? readVectorOperandNG(bytePos, buf, container, false) : NULL;
            VISA_RawOpnd* s0a = cntrls.s0aPresent ? readRawOperandNG(bytePos, buf, container) : NULL;
            VISA_RawOpnd*  oM = cntrls.oMPresent  ? readRawOperandNG(bytePos, buf, container) : NULL;
            VISA_RawOpnd*   R = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd*   G = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd*   B = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd*   A = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd*   Z = cntrls.zPresent ? readRawOperandNG(bytePos, buf, container) : NULL;
            VISA_RawOpnd*   S = cntrls.isStencil ? readRawOperandNG(bytePos, buf, container) : NULL;


            vector<VISA_RawOpnd*> rawOpndVector;
            if (s0a) rawOpndVector.push_back(s0a);
            if ( oM) rawOpndVector.push_back( oM);
            if (  R) rawOpndVector.push_back(  R);
            if (  G) rawOpndVector.push_back(  G);
            if (  B) rawOpndVector.push_back(  B);
            if (  A) rawOpndVector.push_back(  A);
            if (  Z) rawOpndVector.push_back(  Z);
            if (  S) rawOpndVector.push_back(  S);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            kernelBuilderImpl->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
            kernelBuilderImpl->AppendVISA3dRTWriteCPS(pred, emask, esize, rti, cntrls, surfaceHnd, r1HeaderOpnd, sampleIndex, cpsCounter, (uint8_t)rawOpndVector.size(), rawOpndVector.data());
            break;
        }
    case ISA_GATHER4_SCALED:
    case ISA_SCATTER4_SCALED: {
        Common_VISA_EMask_Ctrl eMask = vISA_EMASK_M1;
        Common_ISA_Exec_Size exSize = EXEC_SIZE_ILLEGAL;
        readExecSizeNG(bytePos, buf, exSize, eMask, container);

        VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);
        unsigned channelMask = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
        // scale is ignored and must be zero
        (void) readPrimitiveOperandNG<uint16_t>(bytePos, buf);
        uint8_t surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
        VISA_VectorOpnd*  globalOffset
            = readVectorOperandNG(bytePos, buf, container, false);
        VISA_RawOpnd* offsets = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* dstOrSrc = readRawOperandNG(bytePos, buf, container);

        VISA_StateOpndHandle* surfaceHnd = NULL;
        kernelBuilderImpl
            ->CreateVISAStateOperandHandle(surfaceHnd,
                                           container.surfaceVarDecls[surface]);
        kernelBuilderImpl
            ->AppendVISASurfAccessGather4Scatter4ScaledInst(opcode, pred,
                                                            eMask, exSize,
                                                            ChannelMask::createAPIFromBinary(opcode,
                                                                                             channelMask),
                                                            surfaceHnd,
                                                            globalOffset,
                                                            offsets, dstOrSrc);
        break;
    }
    case ISA_GATHER_SCALED:
    case ISA_SCATTER_SCALED: {
        Common_VISA_EMask_Ctrl eMask = vISA_EMASK_M1;
        Common_ISA_Exec_Size exSize = EXEC_SIZE_ILLEGAL;
        readExecSizeNG(bytePos, buf, exSize, eMask, container);

        VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);
        // block size is ignored (MBZ)
        (void) readPrimitiveOperandNG<uint8_t>(bytePos, buf);
        Common_ISA_SVM_Block_Num  numBlocks =
            Common_ISA_SVM_Block_Num(readPrimitiveOperandNG<uint8_t>(bytePos, buf));
        // scale is ignored (MBZ)
        (void) readPrimitiveOperandNG<uint16_t>(bytePos, buf);
        uint8_t surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
        VISA_VectorOpnd*  globalOffset
            = readVectorOperandNG(bytePos, buf, container, false);
        VISA_RawOpnd* offsets = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* dstOrSrc = readRawOperandNG(bytePos, buf, container);

        VISA_StateOpndHandle* surfaceHnd = NULL;
        kernelBuilderImpl
            ->CreateVISAStateOperandHandle(surfaceHnd,
                                           container.surfaceVarDecls[surface]);
        kernelBuilderImpl
            ->AppendVISASurfAccessScatterScaledInst(opcode, pred,
                                                    eMask, exSize,
                                                    numBlocks,
                                                    surfaceHnd,
                                                    globalOffset,
                                                    offsets, dstOrSrc);
        break;
    }
    case ISA_DWORD_ATOMIC: {
        VISAAtomicOps subOpc;
        unsigned short bitwidth;
        std::tie(subOpc, bitwidth) = getAtomicOpAndBitwidth(bytePos, buf);

        Common_VISA_EMask_Ctrl eMask = vISA_EMASK_M1;
        Common_ISA_Exec_Size exSize = EXEC_SIZE_ILLEGAL;
        readExecSizeNG(bytePos, buf, exSize, eMask, container);

        VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);
        unsigned surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
        VISA_RawOpnd* offsets = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* src0 = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* src1 = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* dst = readRawOperandNG(bytePos, buf, container);

        VISA_StateOpndHandle* surfaceHnd = NULL;
        kernelBuilderImpl
            ->CreateVISAStateOperandHandle(surfaceHnd,
                                           container.surfaceVarDecls[surface]);
        kernelBuilderImpl->AppendVISASurfAccessDwordAtomicInst(
            pred, subOpc, bitwidth == 16, eMask, exSize, surfaceHnd, offsets, src0,
            src1, dst);
        break;
    }
    case ISA_3D_TYPED_ATOMIC: {
        VISAAtomicOps subOpc;
        unsigned short bitwidth;
        std::tie(subOpc, bitwidth) = getAtomicOpAndBitwidth(bytePos, buf);

        Common_VISA_EMask_Ctrl eMask = vISA_EMASK_M1;
        Common_ISA_Exec_Size exSize = EXEC_SIZE_ILLEGAL;
        readExecSizeNG(bytePos, buf, exSize, eMask, container);

        VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);
        unsigned surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
        VISA_RawOpnd* u = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* v = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* r = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* lod = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* src0 = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* src1 = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* dst = readRawOperandNG(bytePos, buf, container);

        VISA_StateOpndHandle* surfaceHnd = NULL;
        kernelBuilderImpl->CreateVISAStateOperandHandle(
            surfaceHnd, container.surfaceVarDecls[surface]);
        kernelBuilderImpl->AppendVISA3dTypedAtomic(subOpc, bitwidth == 16, pred, eMask,
                                                   exSize, surfaceHnd, u, v, r,
                                                   lod, src0, src1, dst);
        break;
    }
    default:
        {
            MUST_BE_TRUE(false, "Unimplemented or Illegal DataPort Opcode.");
        }
    }
}

static void readInstructionControlFlow(unsigned& bytePos, const char* buf, ISA_Opcode opcode, RoutineContainer& container)
{
    VISAKernel* kernelBuilder = container.kernelBuilder;

    Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
    Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;

    switch (opcode)
    {
    case ISA_SUBROUTINE:
    case ISA_LABEL:
        {
            uint16_t labelId = readPrimitiveOperandNG<uint16_t>(bytePos, buf);
            VISA_LabelOpnd* label = container.labelVarDecls[labelId];
            kernelBuilder->AppendVISACFLabelInst(label);
            return;
        }
    case ISA_JMP:
    case ISA_RET:
    case ISA_CALL:
    case ISA_FRET:
    case ISA_FCALL:
        {
            readExecSizeNG(bytePos, buf, esize, emask, container);
            VISA_PredOpnd* pred = hasPredicate(opcode) ? readPredicateOperandNG(bytePos, buf, container) : NULL;

            uint16_t labelId = (opcode == ISA_JMP  || opcode == ISA_CALL || opcode == ISA_FCALL) ?
                readPrimitiveOperandNG<uint16_t>(bytePos, buf) : 0;

            if (opcode == ISA_FCALL)
            {
                uint8_t argSize = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
                uint8_t retSize = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
                kernelBuilder->AppendVISACFFunctionCallInst(pred, emask, esize, container.stringPool[labelId], argSize, retSize);
                return;
            }

            switch (opcode)
            {
            case ISA_JMP  : kernelBuilder->AppendVISACFJmpInst (pred,               container.labelVarDecls[labelId]); return;
            case ISA_CALL : kernelBuilder->AppendVISACFCallInst(pred, emask, esize, container.labelVarDecls[labelId]); return;

            case ISA_RET  : kernelBuilder->AppendVISACFRetInst        (pred, emask, esize); return;
            case ISA_FRET : kernelBuilder->AppendVISACFFunctionRetInst(pred, emask, esize); return;

            default:
                MUST_BE_TRUE(false, "Unimplemented or Illegal Control Flow Opcode.");
                return;

            }

            return;
        }
    case ISA_IFCALL:
    {
        readExecSizeNG(bytePos, buf, esize, emask, container);
        VISA_PredOpnd* pred = hasPredicate(opcode) ? readPredicateOperandNG(bytePos, buf, container) : nullptr;

        VISA_VectorOpnd* funcAddr = readVectorOperandNG(bytePos, buf, container, false);
        uint8_t argSize = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
        uint8_t retSize = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
        kernelBuilder->AppendVISACFIndirectFuncCallInst(pred, emask, esize, funcAddr, argSize, retSize);
        return;
    }
    case ISA_FADDR:
    {
        uint16_t sym_name_idx = readPrimitiveOperandNG<uint16_t>(bytePos, buf);
        VISA_VectorOpnd* dst = readVectorOperandNG(bytePos, buf, container, true);
        kernelBuilder->AppendVISACFSymbolInst(container.stringPool[sym_name_idx], dst);
        return;
    }
    case ISA_SWITCHJMP:
        {
            Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
            Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;
            readExecSizeNG(bytePos, buf, esize, emask, container);

            uint8_t numLabels = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            MUST_BE_TRUE(0 < numLabels && numLabels < 33, "Number of labels in SWITCHJMP must be between 1 and 32.");

            VISA_VectorOpnd* index = readVectorOperandNG(bytePos, buf, container, false);

            VISA_LabelOpnd* labels[32]; /// 32 is max
            for (unsigned i = 0; i < numLabels; i++)
                labels[i] = container.labelVarDecls[readPrimitiveOperandNG<uint16_t>(bytePos, buf)];

            kernelBuilder->AppendVISACFSwitchJMPInst(index, numLabels, labels);
            break;
        }
    default:
        MUST_BE_TRUE(false, "Unimplemented or Illegal Control Flow Opcode.");
    }
}

static void readInstructionMisc(unsigned& bytePos, const char* buf, ISA_Opcode opcode, RoutineContainer& container)
{
    VISAKernel*     kernelBuilder     = container.kernelBuilder;

    switch (opcode)
    {
    case ISA_FILE:
        {
            uint32_t versionInt = getVersionAsInt(container.majorVersion, container.minorVersion);
            bool is3Dot4Plus = versionInt >= getVersionAsInt(3, 4);
            uint32_t filenameIndex = is3Dot4Plus ? readPrimitiveOperandNG<uint32_t>(bytePos, buf) :
                readPrimitiveOperandNG<uint16_t>(bytePos, buf);
            const char* filename = container.stringPool[filenameIndex].c_str();
            kernelBuilder->AppendVISAMiscFileInst((char*)filename);
            break;
        }
    case ISA_LOC:
        {
            unsigned lineNumber = readPrimitiveOperandNG<unsigned>(bytePos, buf);
            kernelBuilder->AppendVISAMiscLOC(lineNumber);
            break;
        }
    case ISA_RAW_SEND:
        {
            uint8_t modifier = readPrimitiveOperandNG<uint8_t>( bytePos, buf);

            Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
            Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;
            readExecSizeNG(bytePos, buf, esize, emask, container);

            VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);

            unsigned exMsgDesc = readPrimitiveOperandNG<unsigned>(bytePos, buf);
            uint8_t     numSrc = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
            uint8_t     numDst = readPrimitiveOperandNG<uint8_t> (bytePos, buf);

            VISA_VectorOpnd* desc = readVectorOperandNG(bytePos, buf, container, false);
            VISA_RawOpnd*     src = readRawOperandNG   (bytePos, buf, container);
            VISA_RawOpnd*     dst = readRawOperandNG   (bytePos, buf, container);

            kernelBuilder->AppendVISAMiscRawSend(pred, emask, esize, modifier, exMsgDesc, numSrc, numDst, desc, src, dst);
            break;
        }
    case ISA_RAW_SENDS:
        {
            uint8_t modifier = readPrimitiveOperandNG<uint8_t>( bytePos, buf);
            bool hasEOT = modifier & 0x2;

            Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
            Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;
            readExecSizeNG(bytePos, buf, esize, emask, container);

            VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);

            uint8_t     numSrc0 = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
            uint8_t     numSrc1 = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
            uint8_t     numDst = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
            uint8_t     ffid = 0;
            if (getVersionAsInt(container.majorVersion, container.minorVersion) >
                    getVersionAsInt(3, 5))
            {
                ffid = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
            }
            VISA_VectorOpnd* exMsgDesc = readVectorOperandNG(bytePos, buf, container, false);

            VISA_VectorOpnd* desc = readVectorOperandNG(bytePos, buf, container, false);
            VISA_RawOpnd*     src0 = readRawOperandNG   (bytePos, buf, container);
            VISA_RawOpnd*     src1 = readRawOperandNG   (bytePos, buf, container);
            VISA_RawOpnd*     dst = readRawOperandNG   (bytePos, buf, container);

            kernelBuilder->AppendVISAMiscRawSends(pred, emask, esize, modifier, ffid, exMsgDesc, numSrc0, numSrc1, numDst, desc, src0, src1, dst, hasEOT);
            break;

        }
    case ISA_VME_FBR:
        {
            VISA_RawOpnd*    UNIInput       = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd*    FBRInput       = readRawOperandNG(bytePos, buf, container);
            uint8_t          surface        = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISA_VectorOpnd* FBRMbMode      = readVectorOperandNG(bytePos, buf, container,false);
            VISA_VectorOpnd* FBRSubMbShape  = readVectorOperandNG(bytePos, buf, container,false);
            VISA_VectorOpnd* FBRSubPredMode = readVectorOperandNG(bytePos, buf, container,false);
            VISA_RawOpnd*    output         = readRawOperandNG(bytePos, buf, container);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
            kernelBuilder->AppendVISAMiscVME_FBR(surfaceHnd, UNIInput, FBRInput, FBRMbMode, FBRSubMbShape, FBRSubPredMode, output);
            break;
        }
    case ISA_VME_IME:
        {
            uint8_t       streamMode = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            uint8_t       searchCtrl = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            VISA_RawOpnd* UNIInput   = readRawOperandNG(bytePos, buf, container);

            VISA_RawOpnd* IMEInput   = readRawOperandNG(bytePos, buf, container);

            uint8_t       surface    = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISA_RawOpnd* ref0       = readRawOperandNG(bytePos, buf, container); //, 2);
            VISA_RawOpnd* ref1       = readRawOperandNG(bytePos, buf, container); //, 2);
            VISA_RawOpnd* costCenter = readRawOperandNG(bytePos, buf, container); //, 4);
            VISA_RawOpnd* output     = readRawOperandNG(bytePos, buf, container);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
            kernelBuilder->AppendVISAMiscVME_IME(surfaceHnd, streamMode, searchCtrl, UNIInput, IMEInput, ref0, ref1, costCenter, output);
            break;
        }
    case ISA_VME_SIC:
        {
            VISA_RawOpnd* UNIInput = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd* SICInput = readRawOperandNG(bytePos, buf, container); //, 128); //SICInput
            uint8_t       surface  = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISA_RawOpnd* output   = readRawOperandNG(bytePos, buf, container);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);

            kernelBuilder->AppendVISAMiscVME_SIC(surfaceHnd, UNIInput, SICInput, output);
            break;
        }
    case ISA_VME_IDM:
        {
            VISA_RawOpnd* UNIInput = readRawOperandNG(bytePos, buf, container); //, 4*32);
            VISA_RawOpnd* IDMInput = readRawOperandNG(bytePos, buf, container); //, 32); //DMInput
            uint8_t       surface  = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISA_RawOpnd* output   = readRawOperandNG(bytePos, buf, container);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);

            kernelBuilder->AppendVISAMiscVME_IDM(surfaceHnd, UNIInput, IDMInput, output);
            break;
        }
    case ISA_3D_URB_WRITE:
        {
            Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
            Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;
            readExecSizeNG(bytePos, buf, esize, emask, container);

            VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);

            uint8_t        numOut = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
            VISA_RawOpnd*  channelMask = readRawOperandNG(bytePos, buf, container);
            uint16_t globalOffset = readPrimitiveOperandNG<uint16_t>(bytePos, buf);

            VISA_RawOpnd*     urbHandle = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd* perSlotOffset = readRawOperandNG(bytePos, buf, container);

            VISA_RawOpnd* vertexData = readRawOperandNG(bytePos, buf, container);

            kernelBuilder->AppendVISA3dURBWrite(pred, emask, esize, numOut, channelMask, globalOffset, urbHandle, perSlotOffset, vertexData);
            break;
        }
    case ISA_LIFETIME:
    {
        VISA_VectorOpnd* opnd = NULL;
        Common_ISA_Operand_Class opndClass;
        VISAVarLifetime lifetime;

        unsigned char properties = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
        uint32_t versionInt = getVersionAsInt(container.majorVersion, container.minorVersion);
        uint32_t varId = versionInt >= getVersionAsInt(3, 4) ? readPrimitiveOperandNG<uint32_t>(bytePos, buf) :
            readPrimitiveOperandNG<uint16_t>(bytePos, buf);

        opndClass = (Common_ISA_Operand_Class)(properties >> 4);
        lifetime = (VISAVarLifetime)(properties & 0x1);

        if(opndClass == OPERAND_GENERAL)
        {
            VISA_GenVar* decl;

            decl = container.generalVarDecls[varId];

            if(lifetime == LIFETIME_START)
            {
                kernelBuilder->CreateVISADstOperand(opnd, decl, 1, 0, 0);
            }
            else
            {
                kernelBuilder->CreateVISASrcOperand(opnd, decl, MODIFIER_NONE, 0, 1, 0, 0, 0);
            }
        }
        else if(opndClass == OPERAND_ADDRESS)
        {
            VISA_AddrVar* decl;
            decl = container.addressVarDecls[varId];

            if(lifetime == LIFETIME_START)
            {
                kernelBuilder->CreateVISAAddressDstOperand(opnd, decl, 0);
            }
            else
            {
                kernelBuilder->CreateVISAAddressSrcOperand(opnd, decl, 0, 1);
            }
        }
        else if(opndClass == OPERAND_PREDICATE)
        {
            VISA_PredVar* decl;

            decl = container.predicateVarDecls[varId];
            VISA_PredOpnd* predOpnd;
            kernelBuilder->CreateVISAPredicateOperand(predOpnd, decl, PredState_NO_INVERSE, PRED_CTRL_NON);
            opnd = (VISA_VectorOpnd*)predOpnd;
        }

        kernelBuilder->AppendVISALifetime(lifetime, opnd);
        break;
    }
    default:
        {
            MUST_BE_TRUE(false, "Unimplemented or Illegal Misc Opcode.");
        }
    }
}

static void readInstructionSVM(unsigned& bytePos, const char* buf, ISA_Opcode opcode, RoutineContainer& container)
{
    Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
    Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;

    VISAKernel*     kernelBuilder     = container.kernelBuilder;

    SVMSubOpcode subOpcode = (SVMSubOpcode)readPrimitiveOperandNG<uint8_t>(bytePos, buf);
    switch (subOpcode)
    {
    case SVM_BLOCK_ST:
    case SVM_BLOCK_LD:
        {
            uint8_t          numOWords = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            bool             unaligned = (numOWords & 8) != 0;
            numOWords &= 7;
            VISA_VectorOpnd*   address = readVectorOperandNG(bytePos, buf, container,false);
            VISA_RawOpnd*          dst = readRawOperandNG(bytePos, buf, container);

            if (subOpcode == SVM_BLOCK_LD)
            {
                kernelBuilder->AppendVISASvmBlockLoadInst(Common_ISA_Oword_Num(numOWords), unaligned, address, dst);
            }
            else
            {
                kernelBuilder->AppendVISASvmBlockStoreInst(Common_ISA_Oword_Num(numOWords), unaligned, address, dst);
            }
            break;
        }
    case SVM_GATHER:
    case SVM_SCATTER:
        {
            readExecSizeNG(bytePos, buf, esize, emask, container);
            VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);

            Common_ISA_SVM_Block_Type blockSize = Common_ISA_SVM_Block_Type(readPrimitiveOperandNG<uint8_t>(bytePos, buf) & 0x3);
            Common_ISA_SVM_Block_Num  numBlocks = Common_ISA_SVM_Block_Num(readPrimitiveOperandNG<uint8_t>(bytePos, buf) & 0x3);
            VISA_RawOpnd* addresses = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd*       dst = readRawOperandNG(bytePos, buf, container);

            if (subOpcode == SVM_GATHER)
            {
                kernelBuilder->AppendVISASvmGatherInst(pred, emask, esize, blockSize, numBlocks, addresses, dst);
            }
            else
            {
                kernelBuilder->AppendVISASvmScatterInst(pred, emask, esize, blockSize, numBlocks, addresses, dst);
            }
            break;
        }
    case SVM_ATOMIC:
        {
            readExecSizeNG(bytePos, buf, esize, emask, container);
            VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);

            VISAAtomicOps op;
            unsigned short bitwidth;
            std::tie(op, bitwidth) = getAtomicOpAndBitwidth(bytePos, buf);

            VISA_RawOpnd* addresses = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd*      src0 = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd*      src1 = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd*       dst = readRawOperandNG(bytePos, buf, container);

            kernelBuilder->AppendVISASvmAtomicInst(
                pred, emask, esize, op, bitwidth, addresses, src0, src1, dst);
            break;
        }
    case SVM_GATHER4SCALED: {
        readExecSizeNG(bytePos, buf, esize, emask, container);
        VISA_PredOpnd *pred = readPredicateOperandNG(bytePos, buf, container);
        unsigned channelMask = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
        // scale is ignored and MBZ
        (void) readPrimitiveOperandNG<uint16_t>(bytePos, buf);
        VISA_VectorOpnd* address = readVectorOperandNG(bytePos, buf, container, false);
        VISA_RawOpnd* offsets = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* dst = readRawOperandNG(bytePos, buf, container);
        kernelBuilder
            ->AppendVISASvmGather4ScaledInst(pred, emask, esize,
                                             ChannelMask::createAPIFromBinary(ISA_SVM,
                                                                              channelMask),
                                             address,
                                             offsets, dst);
        break;
    }
    case SVM_SCATTER4SCALED: {
        readExecSizeNG(bytePos, buf, esize, emask, container);
        VISA_PredOpnd *pred = readPredicateOperandNG(bytePos, buf, container);
        unsigned channelMask = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
        // scale is ignored and MBZ
        (void) readPrimitiveOperandNG<uint16_t>(bytePos, buf);
        VISA_VectorOpnd* address = readVectorOperandNG(bytePos, buf, container, false);
        VISA_RawOpnd* offsets = readRawOperandNG(bytePos, buf, container);
        VISA_RawOpnd* dst = readRawOperandNG(bytePos, buf, container);
        kernelBuilder
            ->AppendVISASvmScatter4ScaledInst(pred, emask, esize,
                                              ChannelMask::createAPIFromBinary(ISA_SVM,
                                                                               channelMask),
                                              address,
                                              offsets, dst);
        break;
    }
    default:
        MUST_BE_TRUE(false, "Unimplemented or Illegal SVM Sub Opcode.");
    }
}

static VISA3DSamplerOp
readSubOpcodeByteNG(unsigned& bytePos, const char* buf)
{
    uint8_t val = 0;
    READ_CISA_FIELD(val, uint8_t, bytePos, buf);
    return VISA3DSamplerOp::extractSamplerOp(val);
}

static VISA_VectorOpnd* readAoffimmi(uint32_t& bytePos, const char* buf, RoutineContainer& container)
{
    VISAKernel*     kernelBuilder = container.kernelBuilder;
    uint32_t versionInt = getVersionAsInt(container.majorVersion, container.minorVersion);
    bool is3Dot4Plus = versionInt >= getVersionAsInt(3, 4);
    VISA_VectorOpnd* aoffimmi = nullptr;
    if (is3Dot4Plus)
    {
        aoffimmi = readVectorOperandNG(bytePos, buf, container, false);
    }
    else
    {
        uint16_t aoffimmiVal = readPrimitiveOperandNG<uint16_t>(bytePos, buf);
        kernelBuilder->CreateVISAImmediate(aoffimmi, &aoffimmiVal, ISA_TYPE_UW);
    }
    return aoffimmi;
}


static void readInstructionSampler(unsigned& bytePos, const char* buf, ISA_Opcode opcode, RoutineContainer& container)
{
    VISAKernel*     kernelBuilder     = container.kernelBuilder;
    VISAKernelImpl* kernelBuilderImpl = ((VISAKernelImpl*)kernelBuilder);

    switch (opcode)
    {
    case ISA_AVS:
        {
            uint8_t chanelMask = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISAChannelMask channel =  ChannelMask::createAPIFromBinary(ISA_AVS, chanelMask);
            uint8_t sampler = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            uint8_t surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            VISA_VectorOpnd* uOffset = readVectorOperandNG(bytePos, buf, container,false);
            VISA_VectorOpnd* vOffset = readVectorOperandNG(bytePos, buf, container,false);
            VISA_VectorOpnd*   deltaU = readVectorOperandNG(bytePos, buf, container,false);
            VISA_VectorOpnd*   deltaV = readVectorOperandNG(bytePos, buf, container,false);
            VISA_VectorOpnd*      u2d = readVectorOperandNG(bytePos, buf, container,false);

            VISA_VectorOpnd*           groupID = readVectorOperandNG(bytePos, buf, container,false);
            VISA_VectorOpnd* verticalBlockNumber = readVectorOperandNG(bytePos, buf, container,false);
            uint8_t                      cntrl = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISA_VectorOpnd*       v2d = readVectorOperandNG(bytePos, buf, container,false);
            uint8_t           execMode = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISA_VectorOpnd* IEFBypass = readVectorOperandNG(bytePos, buf, container,false);
            VISA_RawOpnd* dst = readRawOperandNG(bytePos, buf, container);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            VISA_StateOpndHandle* samplerHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
            kernelBuilder->CreateVISAStateOperandHandle(samplerHnd, container.samplerVarDecls[sampler]);

            kernelBuilder->AppendVISAMEAVS(surfaceHnd, samplerHnd, channel, uOffset, vOffset, deltaU,
                deltaV, u2d, v2d, groupID, verticalBlockNumber, (OutputFormatControl)cntrl, (AVSExecMode)execMode, IEFBypass, dst);
            break;
        }
    case ISA_LOAD:
    case ISA_SAMPLE:
        {
            uint8_t mode = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            uint8_t sampler = (ISA_SAMPLE == opcode) ? readPrimitiveOperandNG<uint8_t>(bytePos, buf) : 0;
            uint8_t surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            VISA_RawOpnd* uOffset = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd* vOffset = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd* roffset = readRawOperandNG(bytePos, buf, container);
            VISA_RawOpnd*     dst = readRawOperandNG(bytePos, buf, container);

            uint8_t  channel = (mode     ) & 0xF;
            bool    isSimd16 = 0x0 != ((mode >> 4) & 0x3);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            VISA_StateOpndHandle* samplerHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);

            if (opcode == ISA_SAMPLE)
            {
                kernelBuilderImpl->CreateVISAStateOperandHandle(samplerHnd, container.samplerVarDecls[sampler]);
                kernelBuilderImpl->AppendVISASISample(vISA_EMASK_M1, surfaceHnd, samplerHnd, ChannelMask::createAPIFromBinary(opcode, channel), isSimd16, uOffset, vOffset, roffset, dst);
            }
            else
                kernelBuilderImpl->AppendVISASILoad(surfaceHnd, ChannelMask::createAPIFromBinary(opcode, channel), isSimd16, uOffset, vOffset, roffset, dst);
            break;
        }
    case ISA_SAMPLE_UNORM:
        {
            uint8_t channelMask = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            uint8_t     sampler = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            uint8_t     surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            CHANNEL_OUTPUT_FORMAT channelOutput = (CHANNEL_OUTPUT_FORMAT)ChannelMask::getChannelOutputFormat(channelMask);

            VISA_VectorOpnd* uOffset = readVectorOperandNG(bytePos, buf, container,false);
            VISA_VectorOpnd* vOffset = readVectorOperandNG(bytePos, buf, container,false);
            VISA_VectorOpnd*  deltaU = readVectorOperandNG(bytePos, buf, container,false);
            VISA_VectorOpnd*  deltaV = readVectorOperandNG(bytePos, buf, container,false);

            VISA_RawOpnd* dst = readRawOperandNG(bytePos, buf, container);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            VISA_StateOpndHandle* samplerHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
            kernelBuilderImpl->CreateVISAStateOperandHandle(samplerHnd, container.samplerVarDecls[sampler]);
            kernelBuilderImpl->AppendVISASISampleUnorm(surfaceHnd, samplerHnd, ChannelMask::createAPIFromBinary(opcode, channelMask), uOffset, vOffset, deltaU, deltaV, dst, channelOutput);
            break;
        }
    case ISA_3D_SAMPLE:
        {
            // 0x6D <op> <pixel_null_mask> <cps_enable> <exec_size> <pred>
            // <channels> <aoffimmi> <sampler> <surface> <dst> <numParams> <params>
            auto op = readSubOpcodeByteNG(bytePos, buf);

            Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
            Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;
            readExecSizeNG(bytePos, buf, esize, emask, container);

            VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);
            uint8_t channelMask = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISA_VectorOpnd* aoffimmi = readAoffimmi(bytePos, buf, container);
            uint8_t sampler = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            uint8_t surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            VISA_RawOpnd*     dst = readRawOperandNG(bytePos, buf, container);
            uint8_t numParams = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            MUST_BE_TRUE(numParams < 16, "number of parameters for 3D_Sample should be < 16");

            VISA_RawOpnd* params[16];
            for (int i = 0; i < numParams; ++i)
            {
                params[i] = readRawOperandNG(bytePos, buf, container);
            }

            VISA_StateOpndHandle* surfaceHnd = NULL;
            VISA_StateOpndHandle* samplerHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
            kernelBuilderImpl->CreateVISAStateOperandHandle(samplerHnd, container.samplerVarDecls[sampler]);
            kernelBuilderImpl->AppendVISA3dSampler(op.opcode, op.pixelNullMask, op.cpsEnable, !op.nonUniformSampler, pred, emask, esize,
                ChannelMask::createAPIFromBinary(opcode, channelMask), aoffimmi, samplerHnd, surfaceHnd, dst, numParams, params);
            break;
        }
    case ISA_3D_LOAD:
        {
            // 0x6E <op> <pixel_null_mask> <exec_size> <pred> <channels>
            // <aoffimmi> <surface> <dst> <numParams> <params>
            // same as 3D_SAMPLE, except that sampler is missing.
            auto op = readSubOpcodeByteNG(bytePos, buf);

            Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
            Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;
            readExecSizeNG(bytePos, buf, esize, emask, container);

            VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);
            uint8_t channelMask = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            VISA_VectorOpnd* aoffimmi = readAoffimmi(bytePos, buf, container);
            uint8_t surface = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            VISA_RawOpnd*     dst = readRawOperandNG(bytePos, buf, container);
            uint8_t numParams = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            MUST_BE_TRUE(numParams < 16, "number of parameters for 3D_Load should be < 16");

            VISA_RawOpnd* params[16];
            for (int i = 0; i < numParams; ++i)
            {
                params[i] = readRawOperandNG(bytePos, buf, container);
            }

            VISA_StateOpndHandle* surfaceHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
            kernelBuilderImpl->AppendVISA3dLoad(op.opcode, op.pixelNullMask, pred, emask, esize,
                ChannelMask::createAPIFromBinary(opcode, channelMask), aoffimmi, surfaceHnd, dst, numParams, params);
            break;
        }
    case ISA_3D_GATHER4:
        {
            auto op = readSubOpcodeByteNG(bytePos, buf);

            Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
            Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;
            readExecSizeNG(bytePos, buf, esize, emask, container);

            VISA_PredOpnd* pred = readPredicateOperandNG(bytePos, buf, container);

            uint8_t   channel = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
            VISA_VectorOpnd* aoffimmi = readAoffimmi(bytePos, buf, container);
            uint8_t   sampler = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
            uint8_t   surface = readPrimitiveOperandNG<uint8_t> (bytePos, buf);

            VISA_RawOpnd* dst = readRawOperandNG(bytePos, buf, container);
            uint8_t numParams = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            MUST_BE_TRUE(numParams < 8, "number of parameters for 3D_Gather4 should be < 8");

            VISA_RawOpnd* params[16];
            for (int i = 0; i < numParams; ++i)
            {
                params[i] = readRawOperandNG(bytePos, buf, container);
            }

            VISA_StateOpndHandle* surfaceHnd = NULL;
            VISA_StateOpndHandle* samplerHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
            kernelBuilderImpl->CreateVISAStateOperandHandle(samplerHnd, container.samplerVarDecls[sampler]);
            kernelBuilder->AppendVISA3dGather4(op.opcode, op.pixelNullMask, pred, emask, esize,
                (VISASourceSingleChannel)channel, aoffimmi, samplerHnd, surfaceHnd, dst, numParams, params);
            break;
        }
    case ISA_3D_INFO:
        {
            VISASampler3DSubOpCode subOpcode = (VISASampler3DSubOpCode)readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            Common_VISA_EMask_Ctrl emask = vISA_EMASK_M1;
            Common_ISA_Exec_Size  esize = EXEC_SIZE_ILLEGAL;
            readExecSizeNG(bytePos, buf, esize, emask, container);
            uint8_t channelMask = 0xF;
            channelMask = readPrimitiveOperandNG<uint8_t>(bytePos, buf);
            uint8_t surface = readPrimitiveOperandNG<uint8_t> (bytePos, buf);

            VISA_RawOpnd* lod = subOpcode == VISA_3D_RESINFO ? readRawOperandNG(bytePos, buf, container) : NULL;
            VISA_RawOpnd* dst = readRawOperandNG(bytePos, buf, container);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);
            kernelBuilder->AppendVISA3dInfo(subOpcode, emask, esize, ChannelMask::createAPIFromBinary(opcode, channelMask), surfaceHnd, lod, dst);

            break;
        }
    case ISA_VA:
        {
            /// subOpcode
            ISA_VA_Sub_Opcode subOpcode = (ISA_VA_Sub_Opcode)readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            VISA_StateOpndHandle* surfaceHnd = NULL;
            VISA_StateOpndHandle* samplerHnd = NULL;
            uint8_t sampler = 0;

            switch(subOpcode)
            {
                case MINMAXFILTER_FOPCODE:
                case Convolve_FOPCODE:
                case Dilate_FOPCODE:
                case ERODE_FOPCODE:
                    {
                        sampler = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
                        kernelBuilderImpl->CreateVISAStateOperandHandle(samplerHnd, container.samplerVarDecls[sampler]);
                        break;
                    }
                default:
                    break; // Prevent gcc warning
            }

            uint8_t surface = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
            kernelBuilder->CreateVISAStateOperandHandle(surfaceHnd, container.surfaceVarDecls[surface]);

            VISA_VectorOpnd* uOffset = readVectorOperandNG(bytePos, buf, container, false);
            VISA_VectorOpnd* vOffset = readVectorOperandNG(bytePos, buf, container,false);

            switch (subOpcode)
            {
            case MINMAX_FOPCODE:
                {
                    /// mmf mode
                    VISA_VectorOpnd* mmfMode = readVectorOperandNG(bytePos, buf, container, false);

                    /// dst
                    VISA_RawOpnd* dst = readRawOperandNG(bytePos, buf, container);
                    kernelBuilder->AppendVISAVAMinMax(surfaceHnd, uOffset, vOffset, mmfMode, dst);
                    break;
                }
            case MINMAXFILTER_FOPCODE:
                {

                    /// cntrl
                    OutputFormatControl cntrl = (OutputFormatControl)readPrimitiveOperandNG<uint8_t>(bytePos, buf);

                    /// execMode
                    MMFExecMode execMode = (MMFExecMode)readPrimitiveOperandNG<uint8_t>(bytePos, buf);

                    /// mmf mode
                    VISA_VectorOpnd* mmfMode = readVectorOperandNG(bytePos, buf, container, false);

                    /// dst
                    VISA_RawOpnd *dst = readRawOperandNG(bytePos, buf, container);
                    kernelBuilder->AppendVISAVAMinMaxFilter(samplerHnd, surfaceHnd, uOffset, vOffset, cntrl, execMode, mmfMode, dst);
                    break;
                }
            case BoolCentroid_FOPCODE:
            case Centroid_FOPCODE:
                {
                    /// v size
                    VISA_VectorOpnd* vSize = readVectorOperandNG(bytePos, buf, container,false);

                    if (subOpcode == BoolCentroid_FOPCODE)
                    {
                        /// h size
                        VISA_VectorOpnd* hSize = readVectorOperandNG(bytePos, buf, container,false);
                        VISA_RawOpnd *dst = readRawOperandNG(bytePos, buf, container);
                        kernelBuilder->AppendVISAVABooleanCentroid(surfaceHnd, uOffset, vOffset, vSize, hSize, dst);
                    }else
                    {
                        VISA_RawOpnd *dst = readRawOperandNG(bytePos, buf, container);
                        kernelBuilder->AppendVISAVACentroid(surfaceHnd, uOffset, vOffset, vSize, dst);

                    }

                    break;
                }
            case Convolve_FOPCODE:
                {
                    /// size for convolve, execMode for erode/dilate
                    uint8_t properties = readPrimitiveOperandNG<uint8_t>(bytePos, buf);

                    bool isBigKernel = (bool)((properties >> 4) & 0x1);
                    CONVExecMode execMode = (CONVExecMode)(properties & 0x3);

                    /// dst
                    VISA_RawOpnd *dst = readRawOperandNG(bytePos, buf, container);
                    kernelBuilder->AppendVISAVAConvolve(samplerHnd, surfaceHnd, uOffset, vOffset, execMode, isBigKernel, dst);
                    break;
                }
            case Dilate_FOPCODE:
            case ERODE_FOPCODE:
                {
                    /// size for convolve, execMode for erode/dilate
                    EDExecMode execMode = (EDExecMode)readPrimitiveOperandNG<uint8_t>(bytePos, buf);
                    EDMode mode = (subOpcode == Dilate_FOPCODE) ? VA_DILATE : VA_ERODE;
                    /// dst
                    VISA_RawOpnd *dst = readRawOperandNG(bytePos, buf, container);
                    kernelBuilder->AppendVISAVAErodeDilate(mode, samplerHnd, surfaceHnd, uOffset, vOffset, execMode, dst);
                    break;
                }
            default:
            break; // Prevent gcc warning
            }

            break;
        }
    case ISA_VA_SKL_PLUS:
        {
            VISA_INST_Desc *instDesc = &CISA_INST_table[opcode];

            /// subOpcode
            ISA_VA_Sub_Opcode subOpcode = (ISA_VA_Sub_Opcode)readPrimitiveOperandNG<uint8_t>(bytePos, buf);

            if( (subOpcode < VA_OP_CODE_1D_CONVOLVE_VERTICAL) ||
                (subOpcode >= VA_OP_CODE_UNDEFINED) )
            {
                ASSERT_USER(false, "Invalid VA sub-opcode");
                return;
            }

#define MAX_NUM_VOPNDS 10
            VISA_VectorOpnd *vOpnds[MAX_NUM_VOPNDS];
            uint8_t numVSrcs = 0;
            memset(vOpnds, 0, sizeof(VISA_VectorOpnd*) * MAX_NUM_VOPNDS);

#define MAX_NUM_MOPNDS 10
            unsigned int miscOpnds[MAX_NUM_MOPNDS];
            uint8_t numMiscOpnds = 0;
            memset(miscOpnds, 0, sizeof(uint32_t) * MAX_NUM_MOPNDS);

            VISA_RawOpnd* dst = NULL;

#define MAX_NUM_RSRCS 5
            VISA_RawOpnd* rawSrcs[MAX_NUM_RSRCS];
            uint8_t numRawSrcs = 0;
            memset(rawSrcs, 0, sizeof(VISA_RawOpnd*) * MAX_NUM_RSRCS);

#define MAX_NUM_SOPNDS 4
            VISA_StateOpndHandle *stateOpnds[MAX_NUM_SOPNDS];
            uint8_t numStateOpnds = 0;
            memset(stateOpnds, 0, sizeof(VISA_StateOpndHandle*) * MAX_NUM_SOPNDS);


            int numTotalOperands = instDesc->getSubInstDesc(subOpcode).opnd_num;

            for(int i = 0; i < numTotalOperands; i++)
            {
                const OpndDesc * opndDesc = &(instDesc->getSubInstDesc(subOpcode).opnd_desc[i]);

                if(opndDesc->opnd_type == OPND_SAMPLE)
                {
                    uint8_t sampler = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
                    kernelBuilderImpl->CreateVISAStateOperandHandle(stateOpnds[numStateOpnds++], container.samplerVarDecls[sampler]);
                } else if(opndDesc->opnd_type == OPND_SURFACE) {
                    uint8_t surface = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
                    kernelBuilderImpl->CreateVISAStateOperandHandle(stateOpnds[numStateOpnds++], container.surfaceVarDecls[surface]);
                } else if((opndDesc->opnd_type & OPND_SRC_GEN) == OPND_SRC_GEN)
                {
                    vOpnds[numVSrcs++] = readVectorOperandNG(bytePos, buf, container, false);
                } else if(opndDesc->opnd_type == OPND_RAW_SRC)
                {
                    rawSrcs[numRawSrcs++] = readRawOperandNG(bytePos, buf, container);
                } else if(opndDesc->opnd_type == OPND_RAW_DST)
                {
                    dst = readRawOperandNG(bytePos, buf, container);
                }
                else if(opndDesc->opnd_type == OPND_OTHER)
                {
                    //in theory this is not necessary since all of them will be UB
                    //but to demonstrate usage model
                    if(opndDesc->data_type == ISA_TYPE_UB)
                    {
                        miscOpnds[numMiscOpnds++] = readPrimitiveOperandNG<uint8_t> (bytePos, buf);
                    }else if(opndDesc->data_type == ISA_TYPE_UW)
                    {
                        miscOpnds[numMiscOpnds++] = readPrimitiveOperandNG<uint16_t> (bytePos, buf);
                    }else if(opndDesc->data_type == ISA_TYPE_UD)
                    {
                        miscOpnds[numMiscOpnds++] = readPrimitiveOperandNG<unsigned int> (bytePos, buf);
                    }else
                    {
                        ASSERT_USER(false, "Invalid misc opnd data type");
                    return;
                    }
                } else {
                    ASSERT_USER(false, "Invalid opnd type");
                    return;
                }
            }
            numVSrcs = 0;
            numRawSrcs = 0;
            numMiscOpnds = 0;
            numStateOpnds = 0;

            switch (subOpcode)
            {
            case VA_OP_CODE_FLOOD_FILL:
                kernelBuilderImpl->AppendVISAVAFloodFill(miscOpnds[0] != 0, rawSrcs[0],
                                                        vOpnds[0], vOpnds[1],
                                                        vOpnds[2], dst);
                break;
            case VA_OP_CODE_1D_CONVOLVE_VERTICAL:
                kernelBuilderImpl->AppendVISAVAConvolve1D(stateOpnds[0], stateOpnds[1], vOpnds[0], vOpnds[1], (CONVExecMode)miscOpnds[0], VA_V_DIRECTION, dst);
                break;
            case VA_OP_CODE_1D_CONVOLVE_HORIZONTAL:
                kernelBuilderImpl->AppendVISAVAConvolve1D(stateOpnds[0], stateOpnds[1], vOpnds[0], vOpnds[1], (CONVExecMode)miscOpnds[0], VA_H_DIRECTION, dst);
                break;
            case VA_OP_CODE_1PIXEL_CONVOLVE:
                kernelBuilderImpl->AppendVISAVAConvolve1Pixel(stateOpnds[0], stateOpnds[1], vOpnds[0], vOpnds[1], (CONV1PixelExecMode)miscOpnds[0], rawSrcs[0], dst);
                break;
            case VA_OP_CODE_LBP_CORRELATION:
                kernelBuilderImpl->AppendVISAVALBPCorrelation(stateOpnds[0], vOpnds[0], vOpnds[1],
                                                        vOpnds[2], dst);
                break;
            case VA_OP_CODE_LBP_CREATION:
                kernelBuilderImpl->AppendVISAVALBPCreation(stateOpnds[0], vOpnds[0], vOpnds[1], (LBPCreationMode)miscOpnds[0], dst);
                break;
            case VA_OP_CODE_CORRELATION_SEARCH:
                kernelBuilderImpl->AppendVISAVACorrelationSearch(stateOpnds[0], vOpnds[0],vOpnds[1],
                                                        vOpnds[2], vOpnds[3], vOpnds[4],
                                                        vOpnds[5], vOpnds[6], vOpnds[7],
                                                        dst);
                break;
            case ISA_HDC_ERODE:
                kernelBuilderImpl->AppendVISAVAHDCErodeDilate(VA_ERODE, stateOpnds[0], stateOpnds[1],
                                                        vOpnds[0], vOpnds[1], stateOpnds[2],vOpnds[2], vOpnds[3]);
                break;
            case ISA_HDC_DILATE:
                kernelBuilderImpl->AppendVISAVAHDCErodeDilate(VA_DILATE, stateOpnds[0], stateOpnds[1],
                                                        vOpnds[0], vOpnds[1], stateOpnds[2],vOpnds[2], vOpnds[3]);
                break;
            case ISA_HDC_LBPCORRELATION:
                kernelBuilderImpl->AppendVISAVAHDCLBPCorrelation(stateOpnds[0], vOpnds[0], vOpnds[1],
                                                        vOpnds[2], stateOpnds[1],
                                                        vOpnds[3], vOpnds[4]);
                break;
            case ISA_HDC_LBPCREATION:
                kernelBuilderImpl->AppendVISAVAHDCLBPCreation(stateOpnds[0], vOpnds[0], vOpnds[1],
                                                        (LBPCreationMode)miscOpnds[0],
                                                        stateOpnds[1], vOpnds[2], vOpnds[3]);
                break;
            case ISA_HDC_MMF:
                kernelBuilderImpl->AppendVISAVAHDCMinMaxFilter(stateOpnds[0], stateOpnds[1],
                                                        vOpnds[0], vOpnds[1], (HDCReturnFormat)miscOpnds[0],
                                                        (MMFEnableMode)miscOpnds[1], stateOpnds[2],
                                                        vOpnds[2], vOpnds[3]);
                break;
            case ISA_HDC_1PIXELCONV:
                kernelBuilderImpl->AppendVISAVAHDCConvolve1Pixel(stateOpnds[0], stateOpnds[1], vOpnds[0], vOpnds[1],
                                                        (HDCReturnFormat)miscOpnds[0], rawSrcs[0], stateOpnds[2],
                                                        vOpnds[2], vOpnds[3]);
                break;
            case ISA_HDC_CONV:
                kernelBuilderImpl->AppendVISAVAHDCConvolve(stateOpnds[0], stateOpnds[1], vOpnds[0],
                                                        vOpnds[1], (HDCReturnFormat)(miscOpnds[0]&0xF),
                                                        (CONVHDCRegionSize)(miscOpnds[0]>>4), stateOpnds[2],
                                                        vOpnds[2], vOpnds[3]);
                break;
            case ISA_HDC_1DCONV_H:
                kernelBuilderImpl->AppendVISAVAHDCConvolve1D(stateOpnds[0], stateOpnds[1], vOpnds[0],
                    vOpnds[1], (HDCReturnFormat)miscOpnds[0], VA_H_DIRECTION,
                    stateOpnds[2], vOpnds[2], vOpnds[3]);
                break;
            case ISA_HDC_1DCONV_V:
                kernelBuilderImpl->AppendVISAVAHDCConvolve1D(stateOpnds[0], stateOpnds[1], vOpnds[0],
                    vOpnds[1], (HDCReturnFormat)miscOpnds[0], VA_V_DIRECTION,
                    stateOpnds[2], vOpnds[2], vOpnds[3]);
                break;
            default:
                ASSERT_USER(false, "Invalid VA sub-opcode");
            }


            break;
        }
    default:
        {
            MUST_BE_TRUE(false, "Unimplemented or Illegal Sampler Opcode.");
        }
    }
}


void readInstructionNG(
    unsigned& bytePos, const char* buf, RoutineContainer& container, unsigned instID)
{
    ISA_Opcode opcode = (ISA_Opcode)readPrimitiveOperandNG<uint8_t>(bytePos, buf);
    MUST_BE_TRUE(opcode < ISA_NUM_OPCODE, "Illegal or unimplemented CISA opcode.");

    //cout << "Opcode: " << ISA_Inst_Table[opcode].str << endl;

    switch(ISA_Inst_Table[opcode].type)
    {
    case ISA_Inst_Mov:
    case ISA_Inst_Sync:
    case ISA_Inst_Arith:
    case ISA_Inst_Logic:
    case ISA_Inst_Address:
    case ISA_Inst_Compare:
    case ISA_Inst_SIMD_Flow: readInstructionCommonNG   (bytePos, buf, (ISA_Opcode)opcode, container); break;
    case ISA_Inst_Data_Port: readInstructionDataportNG (bytePos, buf, (ISA_Opcode)opcode, container); break;
    case ISA_Inst_Flow:      readInstructionControlFlow(bytePos, buf, (ISA_Opcode)opcode, container); break;
    case ISA_Inst_Misc:      readInstructionMisc       (bytePos, buf, (ISA_Opcode)opcode, container); break;
    case ISA_Inst_SVM:       readInstructionSVM        (bytePos, buf, (ISA_Opcode)opcode, container); break;
    case ISA_Inst_Sampler:   readInstructionSampler    (bytePos, buf, (ISA_Opcode)opcode, container); break;
    default:
        {
            stringstream sstr;
            sstr << "Illegal or unimplemented ISA opcode "
                << ISA_Inst_Table[opcode].str
                << " (" << (unsigned)opcode << ")"
                << " at byte position " << bytePos-1
                << "(0x" <<  std::hex   << bytePos-1
                << ")"   << "." << endl;
            MUST_BE_TRUE(false, sstr.str());
        }
    }
}

static void readAttributesNG(uint8_t major, uint8_t minor, unsigned& bytePos, const char* buf, kernel_format_t& header, attribute_info_t* attributes, int numAttributes, vISA::Mem_Manager& mem)
{
    MUST_BE_TRUE(buf    , "Argument Exception: argument buf    is NULL.");

    for (int i = 0; i < numAttributes; i++)
    {
        ASSERT_USER(attributes, "Argument Exception: argument 'attributes' is NULL");

        readVarBytes(major, minor, attributes[i].nameIndex, bytePos, buf);
        READ_CISA_FIELD(attributes[i].size, uint8_t, bytePos, buf);

        const char* attrName = header.strings[attributes[i].nameIndex];
        char* valueBuffer = (char*)mem.alloc(sizeof(char) * (attributes[i].size + 1));
        memcpy_s(valueBuffer, attributes[i].size, &buf[bytePos], attributes[i].size);
        bytePos += attributes[i].size;
        if (strcmp(attrName, "SLMSize")  == 0 ||
            strcmp(attrName, "SurfaceUsage" ) == 0 ||
            strcmp(attrName, "Scope") == 0 ||
            strcmp(attrName, "Target") == 0 ||
            strcmp(attrName, "FESPSize") == 0)
        {
            attributes[i].isInt = true;
            switch (attributes[i].size)
            {
            case 1:
                attributes[i].value.intVal = *valueBuffer;
                break;
            case 2:
                attributes[i].value.intVal = *((short*)valueBuffer);
                break;
            case 4:
                attributes[i].value.intVal = *((int*)valueBuffer);
                break;
            default:
                MUST_BE_TRUE(false, "Unsupported attribute size.");
                break;
            }
        }
        else
        {
            attributes[i].isInt = false;    //by default assume attributes have string value
            attributes[i].value.stringVal = valueBuffer;
            attributes[i].value.stringVal[attributes[i].size] = '\0';
        }
    }
}

static string getDeclLabelString(const char* prefix, uint32_t index, kernel_format_t& header, VISA_Label_Kind kind)
{
    if (index) {
        if (kind == LABEL_FC)
          return header.strings[index];
        stringstream sstr;
        sstr << header.strings[index] << "_" << index;
        return sstr.str();
    }
    return prefix;
}

static void readRoutineNG(unsigned& bytePos, const char* buf, vISA::Mem_Manager& mem, RoutineContainer& container)
{
    kernel_format_t header;
    uint8_t majorVersion = container.majorVersion;
    uint8_t minorVersion = container.minorVersion;

    VISAKernelImpl* kernelBuilderImpl = ((VISAKernelImpl*)container.kernelBuilder);
    bool isKernel = kernelBuilderImpl->getIsKernel();

    unsigned kernelStart = bytePos;

    readVarBytes(majorVersion, minorVersion, header.string_count, bytePos, buf);
    header.strings = (const char**)mem.alloc(header.string_count * sizeof(char*));
    container.stringPool.resize(header.string_count);
    for (unsigned i = 0; i < header.string_count; i++)
    {
        char* str = (char*)mem.alloc(STRING_LEN);
        unsigned j = 0;
        while (buf[bytePos] != '\0' && j < STRING_LEN)
        {
            str[j++] = buf[bytePos++];
        }
        ASSERT_USER(j < STRING_LEN, "string exceeds the maximum length allowed");
        str[j] = '\0';
        bytePos++;
        header.strings[i] = str;
        container.stringPool[i] = str;
    }
    readVarBytes(majorVersion, minorVersion, header.name_index, bytePos, buf);

    /// read general variables
    unsigned numPreDefinedVars = Get_CISA_PreDefined_Var_Count();
    readVarBytes(majorVersion, minorVersion, header.variable_count, bytePos, buf);
    header.variables = (var_info_t*)mem.alloc(sizeof(var_info_t) * (header.variable_count + numPreDefinedVars));
    container.generalVarDecls = (VISA_GenVar**)mem.alloc(sizeof(VISA_GenVar*) * (header.variable_count + numPreDefinedVars));
    container.generalVarsCount = (header.variable_count + numPreDefinedVars);

    for (unsigned i = numPreDefinedVars; i < header.variable_count + numPreDefinedVars; i++)
    {
        unsigned declID = i;
        readVarBytes(majorVersion, minorVersion, header.variables[declID].name_index, bytePos, buf);
        READ_CISA_FIELD(header.variables[declID].bit_properties, uint8_t , bytePos, buf);
        READ_CISA_FIELD(header.variables[declID].num_elements  , uint16_t, bytePos, buf);
        readVarBytes(majorVersion, minorVersion, header.variables[declID].alias_index, bytePos, buf);
        READ_CISA_FIELD(header.variables[declID].alias_offset  , uint16_t, bytePos, buf);

        READ_CISA_FIELD(header.variables[declID].alias_scope_specifier, uint8_t, bytePos, buf);

        READ_CISA_FIELD(header.variables[declID].attribute_count, uint8_t, bytePos, buf);

        header.variables[declID].attributes = (attribute_info_t*)mem.alloc(sizeof(attribute_info_t) * header.variables[declID].attribute_count);
        readAttributesNG(majorVersion, minorVersion, bytePos, buf, header, header.variables[declID].attributes, header.variables[declID].attribute_count, mem);
        header.variables[declID].dcl = NULL;

        /// VISA Builder Call
        var_info_t* var = &header.variables[declID];
        VISA_GenVar* decl = NULL;
        VISA_Type  varType  = (VISA_Type)  ((var->bit_properties     ) & 0xF);
        VISA_Align varAlign = (VISA_Align) ((var->bit_properties >> 4) & 0x7);
        uint8_t aliasScopeSpecifier = header.variables[declID].alias_scope_specifier;
        int status = VISA_SUCCESS;

        assert(aliasScopeSpecifier == 0 && "file scope variables are no longer supported");

        {
            VISA_GenVar* parentDecl = NULL;
            uint16_t aliasOffset = 0;
            uint32_t aliasIndex  = header.variables[declID].alias_index;
            if (aliasIndex > 0)
            {
                if( aliasIndex < numPreDefinedVars )
                {
                   status = kernelBuilderImpl->GetPredefinedVar(parentDecl, (PreDefined_Vars) aliasIndex);
                   ASSERT_USER(status == VISA_SUCCESS, "Invalid index for pre-defined variables");
                }
                else
                {
                    parentDecl = container.generalVarDecls[aliasIndex];
                }
                aliasOffset = header.variables[declID].alias_offset;
            }

            status = kernelBuilderImpl->CreateVISAGenVar(
                decl, header.strings[var->name_index], var->num_elements, varType,
                varAlign, parentDecl, aliasOffset);
            ASSERT_USER(VISA_SUCCESS == status,
                "Failed to add VISA general variable.");
        }

        for (unsigned ai = 0; ai < var->attribute_count; ai++)
        {
            attribute_info_t* attribute = &var->attributes[ai];
            kernelBuilderImpl->AddAttributeToVar(decl, header.strings[attribute->nameIndex], attribute->size, attribute->value.stringVal);
        }

        container.generalVarDecls[i] = decl;
    }

    /// read address variables
    READ_CISA_FIELD(header.address_count, uint16_t, bytePos, buf );
    header.addresses = (addr_info_t*) mem.alloc(sizeof(addr_info_t) * header.address_count);
    container.addressVarDecls = (VISA_AddrVar**)mem.alloc(sizeof(VISA_AddrVar*) * (header.address_count));
    container.addressVarsCount = (header.address_count);
    for (unsigned i = 0; i < header.address_count; i++)
    {
        unsigned declID = i;
        readVarBytes(majorVersion, minorVersion, header.addresses[declID].name_index, bytePos, buf);
        READ_CISA_FIELD(header.addresses[declID].num_elements   , uint16_t, bytePos, buf);
        READ_CISA_FIELD(header.addresses[declID].attribute_count, uint8_t , bytePos, buf);
        header.addresses[declID].attributes = (attribute_info_t*)mem.alloc(sizeof(attribute_info_t) * header.addresses[declID].attribute_count);
        readAttributesNG(majorVersion, minorVersion, bytePos, buf, header, header.addresses[declID].attributes, header.addresses[declID].attribute_count, mem);
        header.addresses[declID].dcl = NULL;

        /// VISA Builder Call
        addr_info_t* var = &header.addresses[declID];
        VISA_AddrVar* decl = NULL;
        int status = kernelBuilderImpl->CreateVISAAddrVar(
            decl, header.strings[var->name_index], var->num_elements);
        ASSERT_USER(VISA_SUCCESS == status,
            "Failed to add VISA address variable.");

        for (unsigned ai = 0; ai < var->attribute_count; ai++)
        {
            attribute_info_t* attribute = &var->attributes[ai];
            kernelBuilderImpl->AddAttributeToVar(decl, header.strings[attribute->nameIndex], attribute->size, attribute->value.stringVal);
        }

        container.addressVarDecls[i] = decl;
    }

    // read predicate variables
    READ_CISA_FIELD(header.predicate_count, uint16_t, bytePos, buf);
    header.predicates = (pred_info_t *) mem.alloc(sizeof(pred_info_t) * (header.predicate_count + COMMON_ISA_NUM_PREDEFINED_PRED));
    container.predicateVarDecls = (VISA_PredVar**)mem.alloc(sizeof(VISA_PredVar*) * (header.predicate_count + COMMON_ISA_NUM_PREDEFINED_PRED));
    container.predicateVarsCount =  (header.predicate_count + COMMON_ISA_NUM_PREDEFINED_PRED);
    for (unsigned i = COMMON_ISA_NUM_PREDEFINED_PRED; i < (unsigned)(header.predicate_count + COMMON_ISA_NUM_PREDEFINED_PRED); i++)
    {
        unsigned declID = i;
        readVarBytes(majorVersion, minorVersion, header.predicates[declID].name_index, bytePos, buf);
        READ_CISA_FIELD(header.predicates[declID].num_elements   , uint16_t, bytePos, buf);
        READ_CISA_FIELD(header.predicates[declID].attribute_count, uint8_t , bytePos, buf);
        header.predicates[declID].attributes = (attribute_info_t*)mem.alloc(sizeof(attribute_info_t) * header.predicates[declID].attribute_count);
        readAttributesNG(majorVersion, minorVersion, bytePos, buf, header, header.predicates[declID].attributes, header.predicates[declID].attribute_count, mem);
        header.predicates[declID].dcl = NULL;

        /// VISA Builder Call
        pred_info_t* var = &header.predicates[declID];
        VISA_PredVar* decl = NULL;
        int status = kernelBuilderImpl->CreateVISAPredVar(
            decl, header.strings[var->name_index], var->num_elements);
        ASSERT_USER(VISA_SUCCESS == status,
            "Failed to add VISA predicate vairable.");

        for (unsigned ai = 0; ai < var->attribute_count; ai++)
        {
            attribute_info_t* attribute = &var->attributes[ai];
            kernelBuilderImpl->AddAttributeToVar(decl, header.strings[attribute->nameIndex], attribute->size, attribute->value.stringVal);
        }

        container.predicateVarDecls[i] = decl;
    }

    // read label variables
    READ_CISA_FIELD(header.label_count, uint16_t, bytePos, buf );
    header.labels = (label_info_t*) mem.alloc(sizeof(label_info_t) * header.label_count);
    container.labelVarDecls = (VISA_LabelOpnd**)mem.alloc(sizeof(VISA_LabelOpnd*) * (header.label_count));
    container.labelVarsCount = header.label_count;
    for (unsigned i = 0; i < header.label_count; i++)
    {
        readVarBytes(majorVersion, minorVersion, header.labels[i].name_index, bytePos, buf );
        READ_CISA_FIELD(header.labels[i].kind, uint8_t, bytePos, buf );
        READ_CISA_FIELD(header.labels[i].attribute_count, uint8_t, bytePos, buf );
        header.labels[i].attributes = (attribute_info_t*) mem.alloc(sizeof(attribute_info_t) * header.labels[i].attribute_count);
        readAttributesNG(majorVersion, minorVersion, bytePos, buf, header, header.labels[i].attributes, header.labels[i].attribute_count, mem);

        /// VISA Builder Call
        unsigned declID = i;
        label_info_t* var = &header.labels[declID];
        VISA_LabelOpnd* decl = NULL;
        int status = kernelBuilderImpl->CreateVISALabelVar(decl,
            getDeclLabelString("L", var->name_index, header, VISA_Label_Kind(var->kind)).c_str(),
            VISA_Label_Kind(var->kind));
        ASSERT_USER(VISA_SUCCESS == status,
            "Failed to add VISA label variable.");

        for (unsigned ai = 0; ai < var->attribute_count; ai++)
        {
            /// TODO: How to Add label decls and attributes correctly.
            ///kernelBuilderImpl->AddAttributeToVar(decl, header.strings[attribute->nameIndex], attribute->size, attribute->value.stringVal);
            ASSERT_USER(false, "Currently the builder API does not support label attributes. Please file a bug.");
        }

        container. labelVarDecls[i] = decl;
    }

    // read sampler variables
    READ_CISA_FIELD(header.sampler_count, uint8_t, bytePos, buf);
    // up to 31 pre-defined samplers are allowed
    MUST_BE_TRUE(header.sampler_count < COMMON_ISA_MAX_NUM_SAMPLERS, "number of vISA samplers exceeds the max");
    header.samplers = (state_info_t*) mem.alloc(sizeof(state_info_t) * COMMON_ISA_MAX_NUM_SAMPLERS);
    container.samplerVarDecls = (VISA_SamplerVar**)mem.alloc(sizeof(VISA_SamplerVar*)* COMMON_ISA_MAX_NUM_SAMPLERS);
    container.samplerVarsCount = header.sampler_count;
    for (unsigned i = 0; i < header.sampler_count; i++)
    {
        readVarBytes(majorVersion, minorVersion, header.samplers[i].name_index, bytePos, buf);
        READ_CISA_FIELD(header.samplers[i].num_elements, uint16_t, bytePos, buf);
        READ_CISA_FIELD(header.samplers[i].attribute_count, uint8_t, bytePos, buf);
        header.samplers[i].attributes = (attribute_info_t *) mem.alloc(sizeof(attribute_info_t) * header.samplers[i].attribute_count);
        readAttributesNG(majorVersion, minorVersion, bytePos, buf, header, header.samplers[i].attributes, header.samplers[i].attribute_count, mem);

        /// VISA Builder Call
        unsigned declID = i;
        state_info_t* var = &header.samplers[declID];
        VISA_SamplerVar* decl = NULL;
        int status = kernelBuilderImpl->CreateVISASamplerVar(
            decl, header.strings[var->name_index], var->num_elements);
        ASSERT_USER(VISA_SUCCESS == status,
            "Failed to add VISA sampler variable.");

        for (unsigned ai = 0; ai < var->attribute_count; ai++)
        {
            attribute_info_t* attribute = &var->attributes[ai];
            kernelBuilderImpl->AddAttributeToVarGeneric(decl, header.strings[attribute->nameIndex], attribute->size, attribute->value.stringVal);
        }

        container.samplerVarDecls[i] = decl;
    }

    kernelBuilderImpl->GetBindlessSampler(container.samplerVarDecls[BINDLESS_SAMPLER_ID]);

    // read surface variables
    READ_CISA_FIELD(header.surface_count, uint8_t, bytePos, buf);
    unsigned num_pred_surf = Get_CISA_PreDefined_Surf_Count();
    header.surface_count += (uint8_t) num_pred_surf;
    header.surface_attrs = (bool*)mem.alloc(sizeof(bool) * header.surface_count);
    memset(header.surface_attrs, 0, sizeof(bool) * header.surface_count);
    header.surfaces = (state_info_t*) mem.alloc(sizeof(state_info_t) * header.surface_count);
    container.surfaceVarDecls = (VISA_SurfaceVar**)mem.alloc(sizeof(VISA_SurfaceVar*) * (header.surface_count));
    container.surfaceVarsCount = header.surface_count;

    /// Populate the predefined surfaces.
    for (unsigned i = 0; i < num_pred_surf; i++)
    {
        VISA_SurfaceVar* surfaceHnd = NULL;
        kernelBuilderImpl->GetPredefinedSurface(surfaceHnd, (PreDefined_Surface)i);
        container.surfaceVarDecls[i] = surfaceHnd;
    }

    /// Populate the rest of the surfaces.
    for (unsigned i = num_pred_surf; i < header.surface_count; i++)
    {
        readVarBytes(majorVersion, minorVersion, header.surfaces[i].name_index, bytePos, buf);
        READ_CISA_FIELD(header.surfaces[i].num_elements, uint16_t, bytePos, buf);
        READ_CISA_FIELD(header.surfaces[i].attribute_count, uint8_t, bytePos, buf);
        header.surfaces[i].attributes = (attribute_info_t *) mem.alloc( sizeof(attribute_info_t) * header.surfaces[i].attribute_count );
        readAttributesNG(majorVersion, minorVersion, bytePos, buf, header, header.surfaces[i].attributes, header.surfaces[i].attribute_count, mem);

        /// VISA Builder Call
        unsigned declID = i;
        state_info_t* var = &header.surfaces[declID];
        VISA_SurfaceVar* decl = NULL;
        int status = kernelBuilderImpl->CreateVISASurfaceVar(
            decl, header.strings[var->name_index], var->num_elements);
        ASSERT_USER(VISA_SUCCESS == status,
            "Failed to add VISA surface variable.");

        for (unsigned ai = 0; ai < var->attribute_count; ai++)
        {
            attribute_info_t* attribute = &var->attributes[ai];

            /// TODO: Does this code even make sense anymore???
            if (!strcmp(header.strings[attribute->nameIndex], "SurfaceUsage"))
            {
                header.surface_attrs[i] = (attribute->value.intVal == 2);
                break;
            }

            kernelBuilderImpl->AddAttributeToVar(decl, header.strings[attribute->nameIndex], attribute->size, attribute->value.stringVal);
        }

        container.surfaceVarDecls[i] = decl;
    }

    int vmeCount = 0;
    READ_CISA_FIELD(vmeCount, uint8_t, bytePos, buf);
    assert(vmeCount == 0 && "VME variable is no longer supported");
    header.vme_count = 0;

    // read input variables
    if (isKernel)
    {
        readVarBytes(container.majorVersion, container.minorVersion, header.input_count, bytePos, buf, FIELD_TYPE::INPUT);

        header.inputs = (input_info_t*)mem.alloc(sizeof(input_info_t) * header.input_count);
        container.inputVarDecls = (CISA_GEN_VAR**)mem.alloc(sizeof(CISA_GEN_VAR*) * (header.input_count));
        container.inputVarsCount = header.input_count;
        for (unsigned i = 0; i < header.input_count; i++)
        {
            READ_CISA_FIELD(header.inputs[i].kind, uint8_t, bytePos, buf);
            readVarBytes(majorVersion, minorVersion, header.inputs[i].index, bytePos, buf);
            READ_CISA_FIELD(header.inputs[i].offset, int16_t, bytePos, buf);
            READ_CISA_FIELD(header.inputs[i].size, uint16_t, bytePos, buf);

            unsigned declID = i;
            input_info_t* var = &header.inputs[declID];
            CISA_GEN_VAR* decl = NULL;

            switch (var->getInputClass())
            {
            case INPUT_GENERAL : decl = container.generalVarDecls[var->index]; break;
            case INPUT_SAMPLER : decl = container.samplerVarDecls[var->index]; break;
            case INPUT_SURFACE : decl = container.surfaceVarDecls[var->index]; break;
            default:
                ASSERT_USER(false, "Incorrect input variable type.");
            }

            int status = kernelBuilderImpl->CreateVISAInputVar(decl, var->offset, var->size, var->getImplicitKind());
            ASSERT_USER(VISA_SUCCESS == status, "Failed to add VISA input variable.");

            container.inputVarDecls[i] = decl;
        }
    }

    READ_CISA_FIELD(header.size , unsigned, bytePos, buf);
    READ_CISA_FIELD(header.entry, unsigned, bytePos, buf);

    if (!isKernel)
    {
        READ_CISA_FIELD(header.input_size       , uint8_t, bytePos, buf);
        READ_CISA_FIELD(header.return_value_size, uint8_t, bytePos, buf);

        // Store size of arg/ret registers for stack call functions
        kernelBuilderImpl->setInputSize( header.input_size );
        kernelBuilderImpl->setReturnSize( header.return_value_size );
    }

    /// read kernel attributes
    READ_CISA_FIELD(header.attribute_count, uint16_t, bytePos, buf);
    header.attributes = (attribute_info_t*)mem.alloc(sizeof(attribute_info_t) * header.attribute_count);
    readAttributesNG(majorVersion, minorVersion, bytePos, buf, header, header.attributes, header.attribute_count, mem);

    bool isTargetSet = false;
    for (unsigned ai = 0; ai < header.attribute_count; ai++)
    {
        attribute_info_t* attribute = &header.attributes[ai];
        /// TODO: This parameter ordering is inconsistent.
        if (attribute->isInt)
        {
            if(strcmp( header.strings[attribute->nameIndex], "Target") == 0)
            {
                isTargetSet = true;
            }
            kernelBuilderImpl->AddKernelAttribute(header.strings[attribute->nameIndex], attribute->size, &attribute->value.intVal  );
        }
        else
        {
            kernelBuilderImpl->AddKernelAttribute(header.strings[attribute->nameIndex], attribute->size, attribute->value.stringVal);
        }
    }
    if(isTargetSet == false)
    {
        VISATarget target = kernelBuilderImpl->getOptions()->getTarget();
        kernelBuilderImpl->AddKernelAttribute("Target", 1, &target);
    }

    unsigned kernelEntry = kernelStart + header.entry;
    unsigned kernelEnd   = kernelEntry + header.size;

    bytePos = kernelEntry;
    unsigned int startBytePos = bytePos;
    bool updateDebugInfo = false;
    if( kernelBuilderImpl->getIsGenBothPath() && kernelBuilderImpl->getOptions()->getOption(vISA_GenerateDebugInfo))
    {
        updateDebugInfo = true;
    }

    for (unsigned i = 0; bytePos < kernelEnd; i++)
    {
        VISAKernel*     kernelBuilder     = container.kernelBuilder;
        VISAKernelImpl* kernelBuilderImpl = ((VISAKernelImpl*)kernelBuilder);
        uint32_t cisaByteOffset;
        if(updateDebugInfo == true)
        {
            cisaByteOffset = bytePos - startBytePos;
        }
        readInstructionNG(bytePos, buf, container, i);
        if(updateDebugInfo == true)
        {
            kernelBuilderImpl->getKernel()->getKernelDebugInfo()->mapCISAOffsetInsert(kernelBuilderImpl->getVISAOffset(), cisaByteOffset);
        }
    }
}

//
// buf -- vISA binary to be processed.  For offline compile it's always the entire vISA object.
//     For JIT mode it's the entire isa file for 3.0, the kernel isa only for 2.x
// builder -- the vISA builder
// kernels -- IR for the vISA kernel
//      if kernelName is specified, return that kernel only in kernels[0]
//      otherwise, all kernels in the isa are processed and returned in kernel
// kernelName -- name of the kernel to be processed.  If null, all kernels will be built
// majorVerion/minorVersion -- version of the vISA binary
// returns true if IR build succeeds, false otherwise
//
extern bool readIsaBinaryNG(const char* buf, CISA_IR_Builder* builder, vector<VISAKernel*> &kernels, const char* kernelName, unsigned int majorVersion, unsigned int minorVersion)
{
    MUST_BE_TRUE(buf, "Argument Exception: argument buf  is NULL.");

    unsigned bytePos = 0;
    vISA::Mem_Manager mem(4096);
    common_isa_header isaHeader;
    isaHeader.num_functions = 0;

    processCommonISAHeader(isaHeader, bytePos, buf, &mem);



    // we have to set the CISA builder version to the binary version,
    // or some instructions that behave differently based on vISA version (e.g., unaligned oword read)
    // would not work correctly
    builder->CISA_IR_setVersion(isaHeader.major_version, isaHeader.minor_version);

    if (kernelName)
    {
        int kernelIndex = -1;
        for (unsigned i = 0; i < isaHeader.num_kernels; i++)
        {
            if (!strcmp(isaHeader.kernels[i].name, kernelName))
            {
                kernelIndex = i;
                break;
            }
        }

        if (kernelIndex == -1)
        {
            return false;
        }

        bytePos = isaHeader.kernels[kernelIndex].offset;

        RoutineContainer container;
        container.builder = builder;
        container.kernelBuilder = NULL;
        container.majorVersion = isaHeader.major_version;
        container.minorVersion = isaHeader.minor_version;

        builder->AddKernel(container.kernelBuilder, isaHeader.kernels[kernelIndex].name);

        VISAKernelImpl* kernelImpl = (VISAKernelImpl*)container.kernelBuilder;
        kernelImpl->setIsKernel(true);
        kernels.push_back(container.kernelBuilder);

        readRoutineNG(bytePos, buf, mem, container);

        for (unsigned int i = 0; i < isaHeader.num_functions; i++)
        {
            bytePos = isaHeader.functions[i].offset;

            VISAFunction* funcPtr = NULL;
            builder->AddFunction(funcPtr, isaHeader.functions[i].name);

            container.kernelBuilder = (VISAKernel*)funcPtr;

            ((VISAKernelImpl*)container.kernelBuilder)->setIsKernel(false);
            kernels.push_back(container.kernelBuilder);

            readRoutineNG(bytePos, buf, mem, container);
        }
    }
    else
    {
        for( unsigned int k = 0; k < isaHeader.num_kernels; k++ )
        {
            bytePos = isaHeader.kernels[k].offset;

            RoutineContainer container;
            container.builder = builder;
            container.kernelBuilder = NULL;
            container.majorVersion = isaHeader.major_version;
            container.minorVersion = isaHeader.minor_version;

            builder->AddKernel(container.kernelBuilder, isaHeader.kernels[k].name);

            ((VISAKernelImpl*)container.kernelBuilder)->setIsKernel(true);
            kernels.push_back(container.kernelBuilder);

            readRoutineNG(bytePos, buf, mem, container);
        }

        for (unsigned int i = 0; i < isaHeader.num_functions; i++)
            {
                RoutineContainer container;

                container.builder = builder;
                container.majorVersion = isaHeader.major_version;
                container.minorVersion = isaHeader.minor_version;

                bytePos = isaHeader.functions[i].offset;

                VISAFunction* funcPtr = NULL;
                builder->AddFunction(funcPtr, isaHeader.functions[i].name);

                container.kernelBuilder = (VISAKernel*)funcPtr;

                ((VISAKernelImpl*)container.kernelBuilder)->setIsKernel(false);
                kernels.push_back(container.kernelBuilder);

                readRoutineNG(bytePos, buf, mem, container);
            }
    }

    return true;
}

