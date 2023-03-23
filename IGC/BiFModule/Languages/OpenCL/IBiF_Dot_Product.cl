/*========================== begin_copyright_notice ============================

Copyright (C) 2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#ifdef cl_khr_integer_dot_product
//===-  IBiF_Dot_Product.cl -===============================================//
//
// This file defines OpenCL dot product functions.
// These functions are part of the KHR cl_khr_integer_dot_product extension.
//
//===--------------------------------------------------------------------===//

// Supported Caps related to the cl_khr_integer_dot_product extension
#define __opencl_c_dot_product_signed
#define __opencl_c_dot_product_mixed_signedness
#define __opencl_c_integer_dot_product_saturation_accumulation
#define __opencl_c_integer_dot_product_input_4x8bit_packed

// Currently support CL_DEVICE_INTEGER_DOT_PRODUCT_INPUT_4x8BIT_KHR only

#define  DEFN_INTEL_DOT_PRODUCT(TYPE, TYPE_ARG1, TYPE_ARG2, TYPE_SUFFIX, ARG_SUFFIX)        \
INLINE TYPE OVERLOADABLE dot( TYPE_ARG1 a, TYPE_ARG2 b )                                    \
{                                                                                           \
    return __builtin_spirv_Op##TYPE_SUFFIX##DotKHR_##ARG_SUFFIX##_##ARG_SUFFIX(a, b);       \
}

#define  DEFN_INTEL_DOT_PRODUCT_SAT(TYPE, TYPE_ARG1, TYPE_ARG2, TYPE_SUFFIX, ARG_SUFFIX, ACC_SUFFIX)                \
INLINE TYPE OVERLOADABLE  dot_acc_sat( TYPE_ARG1 a, TYPE_ARG2 b, TYPE acc )                                         \
{                                                                                                                   \
    return __builtin_spirv_Op##TYPE_SUFFIX##DotAccSatKHR_##ARG_SUFFIX##_##ARG_SUFFIX##_##ACC_SUFFIX(a, b, acc);     \
}

#define DEFN_INTEL_DOT_PRODUCT_BUILTIN_SPIRV(TYPE, TYPE_ARG1, TYPE_ARG2, TYPE_SUFFIX, ARG_SUFFIX, TYPE_SUFFIX_IB)   \
TYPE __builtin_spirv_Op##TYPE_SUFFIX##DotKHR_##ARG_SUFFIX##_##ARG_SUFFIX(TYPE_ARG1 a, TYPE_ARG2 b)                  \
{                                                                                                                   \
    union { int _i; TYPE_ARG1 _arg1; TYPE_ARG2 _arg2 } a1, a2;                                                      \
    a1._arg1 = a;                                                                                                   \
    a2._arg2 = b;                                                                                                   \
    return __builtin_IB_dp4a_##TYPE_SUFFIX_IB(0, a1._i, a2._i, false);                                              \
}

#define DEFN_INTEL_DOT_PRODUCT_SAT_BUILTIN_SPIRV(TYPE, TYPE_ARG1, TYPE_ARG2, TYPE_SUFFIX, ARG_SUFFIX, ACC_SUFFIX, TYPE_SUFFIX_IB)  \
TYPE __builtin_spirv_Op##TYPE_SUFFIX##DotAccSatKHR_##ARG_SUFFIX##_##ARG_SUFFIX##_##ACC_SUFFIX(TYPE_ARG1 a, TYPE_ARG2 b, TYPE acc)  \
{                                                                                                                                  \
    union { int _i; TYPE_ARG1 _arg1; TYPE_ARG2 _arg2 } a1, a2;                                                                     \
    a1._arg1 = a;                                                                                                                  \
    a2._arg2 = b;                                                                                                                  \
    return __builtin_IB_dp4a_##TYPE_SUFFIX_IB(acc, a1._i, a2._i, true);                                                            \
}

#define DEFN_INTEL_DOT_PRODUCT_US(TYPE, ARG_TYPE, ARG_SUFFIX)               \
INLINE TYPE OVERLOADABLE dot( u##ARG_TYPE a, ARG_TYPE b )                   \
{                                                                           \
    return __builtin_spirv_OpSUDotKHR_##ARG_SUFFIX##_##ARG_SUFFIX(b, a);    \
}

#define DEFN_INTEL_DOT_PRODUCT_SAT_US(TYPE, ARG_TYPE, ARG_SUFFIX, ACC_SUFFIX)                       \
INLINE TYPE OVERLOADABLE dot_acc_sat( u##ARG_TYPE a, ARG_TYPE b, TYPE acc )                         \
{                                                                                                   \
    return __builtin_spirv_OpSUDotAccSatKHR_##ARG_SUFFIX##_##ARG_SUFFIX##_##ACC_SUFFIX(b, a, acc);  \
}

#define DEFN_INTEL_DOT_PRODUCT_PACKED_BUILTIN_SPIRV(TYPE, TYPE_SUFFIX, TYPE_SUFFIX_IB)                          \
TYPE __builtin_spirv_Op##TYPE_SUFFIX##DotKHR_i32_i32_i32(uint a, uint b, int packed)                            \
{                                                                                                               \
    union { int _i; uint _arg1; uint _arg2 } a1, a2;                                                            \
    a1._arg1 = a;                                                                                               \
    a2._arg2 = b;                                                                                               \
    return __builtin_IB_dp4a_##TYPE_SUFFIX_IB(0, a1._i, a2._i, false);                                          \
}

#define DEFN_INTEL_DOT_PRODUCT_SAT_PACKED_BUILTIN_SPIRV(TYPE, TYPE_SUFFIX, TYPE_SUFFIX_IB, SAT_PREFIX)    \
TYPE __builtin_spirv_Op##TYPE_SUFFIX##DotAccSatKHR_i32_i32_i32_i32(uint a, uint b, TYPE acc, int packed)  \
{                                                                                                         \
    union { int _i; uint _arg1; uint _arg2 } a1, a2;                                                      \
    a1._arg1 = a;                                                                                         \
    a2._arg2 = b;                                                                                         \
    return __builtin_IB_dp4a_##TYPE_SUFFIX_IB(acc, a1._i, a2._i, true);                                   \
}

#define  DEFN_INTEL_DOT_PRODUCT_PACKED(TYPE, ARG_TYPES, TYPE_SUFFIX)            \
INLINE TYPE OVERLOADABLE dot_4x8packed_##ARG_TYPES##_##TYPE( uint a, uint b)    \
{                                                                               \
    return __builtin_spirv_Op##TYPE_SUFFIX##DotKHR_i32_i32_i32(a, b, 0);        \
}

#define  DEFN_INTEL_DOT_PRODUCT_SAT_PACKED(TYPE, TYPE_SUFFIX, ARG_TYPES)                                        \
INLINE TYPE OVERLOADABLE  dot_acc_sat_4x8_packed_##ARG_TYPES##_##TYPE( uint a, uint b, TYPE acc)                \
{                                                                                                               \
    return __builtin_spirv_Op##TYPE_SUFFIX##DotAccSatKHR_i32_i32_i32_i32(a, b, acc, 0);                         \
}

#define DEFN_INTEL_DOT_PRODUCT_PACKED_US                            \
INLINE int OVERLOADABLE dot_4x8packed_us_int( uint a, uint b)       \
{                                                                   \
    return __builtin_spirv_OpSUDotKHR_i32_i32_i32(b, a, 0);         \
}

#define DEFN_INTEL_DOT_PRODUCT_SAT_PACKED_US                                        \
INLINE int OVERLOADABLE dot_acc_sat_4x8packed_us_int( uint a, uint b, int acc)      \
{                                                                                   \
    return __builtin_spirv_OpSUDotAccSatKHR_i32_i32_i32_i32(b, a, acc, 0);          \
}

DEFN_INTEL_DOT_PRODUCT_BUILTIN_SPIRV(uint, uchar4, uchar4, U, v4i8, uu)
DEFN_INTEL_DOT_PRODUCT(uint, uchar4, uchar4, U, v4i8)
DEFN_INTEL_DOT_PRODUCT_BUILTIN_SPIRV(uint, ushort2, ushort2, U, v2i16, uu)
DEFN_INTEL_DOT_PRODUCT(uint, ushort2, ushort2, U, v2i16)
#ifdef __opencl_c_integer_dot_product_input_4x8bit_packed
DEFN_INTEL_DOT_PRODUCT_PACKED_BUILTIN_SPIRV(uint, U, uu)
DEFN_INTEL_DOT_PRODUCT_PACKED(uint, uu, U)
#endif // __opencl_c_integer_dot_product_input_4x8bit_packed
#ifdef __opencl_c_integer_dot_product_saturation_accumulation
DEFN_INTEL_DOT_PRODUCT_SAT_BUILTIN_SPIRV(uint, uchar4, uchar4, U, v4i8, i32, uu)
DEFN_INTEL_DOT_PRODUCT_SAT(uint, uchar4, uchar4, U, v4i8, i32)
DEFN_INTEL_DOT_PRODUCT_SAT_BUILTIN_SPIRV(uint, ushort2, ushort2, U, v2i16, i32, uu)
DEFN_INTEL_DOT_PRODUCT_SAT(uint, ushort2, ushort2, U, v2i16, i32)
#ifdef __opencl_c_integer_dot_product_input_4x8bit_packed
DEFN_INTEL_DOT_PRODUCT_SAT_PACKED_BUILTIN_SPIRV(uint, U, uu, u)
DEFN_INTEL_DOT_PRODUCT_SAT_PACKED(uint, U, uu)
#endif // __opencl_c_integer_dot_product_input_4x8bit_packed
#endif // __opencl_c_integer_dot_product_saturation_accumulation

#ifdef __opencl_c_dot_product_signed
DEFN_INTEL_DOT_PRODUCT_BUILTIN_SPIRV(int, char4, char4, S, v4i8, ss)
DEFN_INTEL_DOT_PRODUCT(int, char4, char4, S, v4i8)
DEFN_INTEL_DOT_PRODUCT_BUILTIN_SPIRV(int, short2, short2, S, v2i16, ss)
DEFN_INTEL_DOT_PRODUCT(int, short2, short2, S, v2i16)
#ifdef __opencl_c_integer_dot_product_input_4x8bit_packed
DEFN_INTEL_DOT_PRODUCT_PACKED_BUILTIN_SPIRV(int, S, ss)
DEFN_INTEL_DOT_PRODUCT_PACKED(int, ss, S)
#endif // __opencl_c_integer_dot_product_input_4x8bit_packed
#ifdef __opencl_c_integer_dot_product_saturation_accumulation
DEFN_INTEL_DOT_PRODUCT_SAT_BUILTIN_SPIRV(int, char4, char4, S, v4i8, i32, ss)
DEFN_INTEL_DOT_PRODUCT_SAT(int, char4, char4, S, v4i8, i32)
DEFN_INTEL_DOT_PRODUCT_SAT_BUILTIN_SPIRV(int, short2, short2, S, v2i16, i32, ss)
DEFN_INTEL_DOT_PRODUCT_SAT(int, short2, short2, S, v2i16, i32)
#ifdef __opencl_c_integer_dot_product_input_4x8bit_packed
DEFN_INTEL_DOT_PRODUCT_SAT_PACKED_BUILTIN_SPIRV(int, S, ss, s)
DEFN_INTEL_DOT_PRODUCT_SAT_PACKED(int, S, ss)
#endif // __opencl_c_integer_dot_product_input_4x8bit_packed
#endif // __opencl_c_integer_dot_product_saturation_accumulation
#endif // __opencl_c_dot_product_signed

#ifdef __opencl_c_dot_product_mixed_signedness
DEFN_INTEL_DOT_PRODUCT_BUILTIN_SPIRV(int, char4, uchar4, SU, v4i8, su)
DEFN_INTEL_DOT_PRODUCT(int, char4, uchar4, SU, v4i8)
DEFN_INTEL_DOT_PRODUCT_US(int, char4, v4i8)
DEFN_INTEL_DOT_PRODUCT_BUILTIN_SPIRV(int, short2, ushort2, SU, v2i16, su)
DEFN_INTEL_DOT_PRODUCT(int, short2, ushort2, SU, v2i16)
DEFN_INTEL_DOT_PRODUCT_US(int, short2, v2i16)
#ifdef __opencl_c_integer_dot_product_input_4x8bit_packed
DEFN_INTEL_DOT_PRODUCT_PACKED_BUILTIN_SPIRV(int, SU, su)
DEFN_INTEL_DOT_PRODUCT_PACKED(int, su, SU)
DEFN_INTEL_DOT_PRODUCT_PACKED_US
#endif // __opencl_c_integer_dot_product_input_4x8bit_packed
#ifdef __opencl_c_integer_dot_product_saturation_accumulation
DEFN_INTEL_DOT_PRODUCT_SAT_BUILTIN_SPIRV(int, char4, uchar4, SU, v4i8, i32, su)
DEFN_INTEL_DOT_PRODUCT_SAT(int, char4, uchar4, SU, v4i8, i32)
DEFN_INTEL_DOT_PRODUCT_SAT_US(int, char4, v4i8, i32)
DEFN_INTEL_DOT_PRODUCT_SAT_BUILTIN_SPIRV(int, short2, ushort2, SU, v2i16, i32, su)
DEFN_INTEL_DOT_PRODUCT_SAT(int, short2, ushort2, SU, v2i16, i32)
DEFN_INTEL_DOT_PRODUCT_SAT_US(int, short2, v2i16, i32)
#ifdef __opencl_c_integer_dot_product_input_4x8bit_packed
DEFN_INTEL_DOT_PRODUCT_SAT_PACKED_BUILTIN_SPIRV(int, SU, su, s)
DEFN_INTEL_DOT_PRODUCT_SAT_PACKED(int, SU, su)
DEFN_INTEL_DOT_PRODUCT_SAT_PACKED_US
#endif // __opencl_c_integer_dot_product_input_4x8bit_packed
#endif // __opencl_c_integer_dot_product_saturation_accumulation
#endif // __opencl_c_dot_product_mixed_signedness

// For possible future support of CL_DEVICE_INTEGER_DOT_PRODUCT_INPUT_ALL_KHR
// extension, one needs to:
// - Add arguments of 'short/ushort' type and 'long' result type
// - Add different vector sizes such as 2,3,8,16
#endif // cl_khr_integer_dot_product
