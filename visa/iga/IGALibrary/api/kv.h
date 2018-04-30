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
#ifndef IGA_KV_H
#define IGA_KV_H

#include "iga.h"

/*************************************************************************
*                                                                       *
*                  The KernelView C interface                           *
*                                                                       *
*************************************************************************/

#ifdef __cplusplus
extern "C"  {
#endif

/*
* This symbols defines the maximum number of PC targets that an instruction
* may have.  It is typically used to statically allocate an array of target
* PCs with the kv_get_inst_targets function.
* E.g.
*   uint32_t targetPCs[KV_MAX_TARGETS_PER_INSTRUCTION];
*   uint32_t num = kv_get_inst_targets(kv, atPc, &targets[0]);
*   for (int i = 0; i < num; i++) {
*      processTarget(targetPCs[i]);
*   }
*
*/
#define KV_MAX_TARGETS_PER_INSTRUCTION 3
/*
* This symbol represents an invalid PC.  0 is a valid PC (the beginnning
* of the kernel).
*/
#define KV_INVALID_PC ((int32_t)0xFFFFFFFF)

/* incomplete type for a kernel view handle */
struct kv_t;


/*
* Creates a kernel view.
*   'plat' - the platform
*   'bytes' - the kernel binary
*   'bytes_len' - the length of 'bytes'
*   'status' - the IGA status code
*   'errbuf' - an optional buffer to emit errors or warnings (can pass nullptr)
*   'errbuf_cap' - the capacity of errbuf.
* RETURNS: a kernel view object for use in other kv_* functions.
* Deallocate it with kv_delete.  If there is a decode error (or other errors), this
* function returns an instance of Kernel Views and ERROR status. If user proceeds
* to use the returned Kernel View we do not guarantee that all bits are correct
*/
IGA_API kv_t *kv_create(
    iga_gen_t plat,
    const void *bytes,
    size_t bytes_len,
    iga_status_t *status,
    char *errbuf,
    size_t errbuf_cap);

/* destroys a kernel view */
IGA_API void kv_delete(kv_t *);


/*
* Returns the size of the instruction at 'pc'; returns 0 if the program
* address is out of bounds.  This allows one to iterate a kernel using this
* API.  For example:
*
*   uint32_t iLen;
*   for (uint32_t pc = 0;
*        (iLen = kv_get_inst_size(kv, pc)) != 0;
*        pc += iLen)
*   {
*     ... process instruction
*   }
*/
IGA_API int32_t kv_get_inst_size(const kv_t *kv, int32_t pc);


/*
* This function returns the absolute PC targets of this instruction.
* For branching instructions, it populates 'pcs' with the jump targets
* of this instruction.  The number of PC's will always be less than or
* equal to MAX_KV_TARGETS_COUNT.  The function returns the number of
* target PCs populated in the 'pcs' argument.
*
* For non-branching instructions this returns 0 and does not touch 'pcs'.
*
* If 'pcs' is NULL, it is ignored.  The number of targets is still returned.
*/
IGA_API uint32_t kv_get_inst_targets(
    const kv_t *kv,
    int32_t pc,
    int32_t *pcs);


/*
* This function returns the syntax for a given instruction.
* The user passes the buffer 'sbuf' (along with its capacity) to hold
* the output.
*
* The optional 'get_label_name' callback converts a PC into a label.
* The caller can provide NULL and internal label names will be used.
* The 'env' context parameter is passed to 'get_label_name'.
* Memory returned by the callback is only read.  If the callback allocates,
* then the caller of this function must cleanup.
*/
IGA_API size_t kv_get_inst_syntax(
    const kv_t *kv,
    int32_t pc,
    char *sbuf,
    size_t sbuf_cap,
    const char *(*get_label_name)(int32_t, void *),
    void *env);

/*
* This function returns the default label name if custom labeler is not used.
*/
IGA_API size_t kv_get_default_label_name(
    int32_t pc,
    char *sbuf,
    size_t sbuf_cap);

/*
* Returns non-zero iff this instruction is a branch target.
* The caller can use this function to determine if it should emit a label
* first.
*/
IGA_API uint32_t kv_is_inst_target(const kv_t *kv, int32_t pc);


/*
* This enumeration allows one to determine if a given PC is for structured
* control flow.  This is for tools that want to render an indentation for
* readability.
*/
typedef enum {
    KV_OPGROUP_INVALID,   /* not a valid op (e.g. out of bounds, middle of instruction) */
    KV_OPGROUP_OTHER,     /* some other instruction */
    KV_OPGROUP_IF,        /* an 'if' op */
    KV_OPGROUP_ELSE,      /* an 'else' op */
    KV_OPGROUP_ENDIF,     /* an 'endif' op */
    KV_OPGROUP_WHILE,     /* a 'while' op */
    KV_OPGROUP_SEND_EOT,  /* a send message with the EOT bit set */
} kv_opgroup_t;


/*
* This function returns the opcode group.  The result may be compared
* to the integral value of the various kv_opcode_group enumerates.
* (See enum kv_get_opgroup_t.)
*/
IGA_API int32_t kv_get_opgroup(const kv_t *kv, int32_t pc);


/*
* Returns the send function descriptors.  The count of descriptors is
* returned; hence, if the instruction is invalid or not a send or
* send using two index registers, 0 is returned.
* If one of the descriptors is not immediate, then 1 is returned
* and that descriptor is set to KV_INVALID_SEND_DESC.
*
* Also returns 0 if any parameter is NULL (and parameters are untouched).
*/
IGA_API uint32_t kv_get_send_descs(
    const kv_t *kv,
    int32_t pc,
    uint32_t *ex_desc,
    uint32_t *desc);
/*
* A symbol to indicate an invalid send descriptor value.
*/
#define KV_INVALID_SEND_DESC ((uint32_t)0xFFFFFFFFF)

/* TODO: review necessity of this macro.
* A symbol to indicate an invalid message length value.
*/
#define KV_INVALID_LEN ((uint32_t)0xFFFFFFFFF)


/*
* Returns message type for the following SFID:
* Sampler, DP_CC, DP_DC0, DP_DC1, DP_DC2, DP_RC, DP_DCR0
*/
IGA_API int32_t kv_get_message_type(const kv_t *kv, int32_t pc);

/*
* Returns message sfid.
*/
IGA_API int32_t kv_get_message_sfid(const kv_t *kv, int32_t pc);

/*
* Sets message length, extended message length, and response length in units of registers.
* The count of lengths successfully set is returned. If any of the parameters is NULL,
* it returns 0. Invalid lengths are set to KV_INVALID_LEN.
*/
IGA_API uint32_t kv_get_message_len(const kv_t *kv, int32_t pc, uint32_t* mLen, uint32_t* emLen, uint32_t* rLen);

/*
* Returns Execution size of the instruction
* 0 - INVALID
* 1 - EXEC_SIZE_1
* 2 - EXEC_SIZE_2
* 3 - EXEC_SIZE_4
* 4 - EXEC_SIZE_8
* 5 - EXEC_SIZE_16
* 6 - EXEC_SIZE_32
*/
IGA_API uint32_t kv_get_execution_size(const kv_t *kv, int32_t pc);


/*
* Returns number of sources this instruction has.
*/
IGA_API int32_t kv_get_number_sources(const kv_t *kv, int32_t pc);

/*
* This function returns OPcode integer.  The value corresponds to
* binary encoding value of the opcode.
*/
IGA_API uint32_t kv_get_opcode(const kv_t *kv, int32_t pc);

/*
* This function returns if intruction has destination.
*/
IGA_API int32_t kv_get_has_destination(const kv_t *kv, int32_t pc);

/*
* This function returns destination Register row
*/
IGA_API int32_t kv_get_destination_register(const kv_t *kv, int32_t pc);

/*
* This function returns destination subRegister
*/
IGA_API int32_t kv_get_destination_sub_register(const kv_t *kv, int32_t pc);

/*
* This function returns destination data type
* i.e. F, HF, INT, etc
*/
IGA_API uint32_t kv_get_destination_data_type(const kv_t *kv, int32_t pc);

/*
* This function returns destination register type
* i.e. GRF, various ARF registers
*/
IGA_API uint32_t kv_get_destination_register_type(const kv_t *kv, int32_t pc);

/*
* This function returns destination register KIND
* DIRECT, INDIRECT, IMM, INDIR etc
*/
IGA_API uint32_t kv_get_destination_register_kind(const kv_t *kv, int32_t pc);

/*
* This function returns source register line number for a given source.
*/
IGA_API int32_t kv_get_source_register(const kv_t *kv, int32_t pc, uint32_t sourceNumber);

/*
* This function returns source subRegister for a given source.
*/
IGA_API int32_t kv_get_source_sub_register(const kv_t *kv, int32_t pc, uint32_t sourceNumber);

/*
* This function returns source data type for a given source
* i.e. F, HF, INT, etc
*/
IGA_API uint32_t kv_get_source_data_type(const kv_t *kv, int32_t pc, uint32_t sourceNumber);

/*
* This function returns source register type for a given source.
* i.e. GRF, various ARF registers
*/
IGA_API uint32_t kv_get_source_register_type(const kv_t *kv, int32_t pc, uint32_t sourceNumber);

/*
* This function returns source register KIND for a given source
* DIRECT, INDIRECT, IMM, INDIR etc
*/
IGA_API uint32_t kv_get_source_register_kind(const kv_t *kv, int32_t pc, uint32_t sourceNumber);

/*
* This function returns whether source is a vector.
*/
IGA_API int32_t kv_is_source_vector(const kv_t *kv, int32_t pc, uint32_t sourceNumber);

/*
* This function returns mask offset
*/
IGA_API uint32_t kv_get_channel_offset(const kv_t *kv, int32_t pc);

/*
* This function returns mask control
*/
IGA_API uint32_t kv_get_mask_control(const kv_t *kv, int32_t pc);

/*
* This function exposes destination region.
*/
IGA_API int32_t kv_get_destination_region(const kv_t *kv, int32_t pc, uint32_t *hz);

/*
* This function exposes source operand region.
*/
IGA_API int32_t kv_get_source_region(const kv_t *kv, int32_t pc, uint32_t src_op, uint32_t *vt, uint32_t *wi, uint32_t *hz);

/*
* This function exposes source operand immediate value.
*/
IGA_API int32_t kv_get_source_immediate(const kv_t *kv, int32_t pc, uint32_t src_op, uint64_t *imm);

#ifdef __cplusplus
}
#endif
#endif
