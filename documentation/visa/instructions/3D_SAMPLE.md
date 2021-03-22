<!---======================= begin_copyright_notice ============================

Copyright (c) 2019-2021 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

============================= end_copyright_notice ==========================-->

 

## Opcode

  3D_SAMPLE = 0x6d

## Format


### CONDITION

- Op.op=LOD, sample, sample_lz


| | | | | | | |
| --- | --- | --- | --- | --- | --- | --- |
| 0x6d(3D_SAMPLE) | Op            | Exec_size | Pred      | Channels | Aoffimmi | Sampler |
|                 | Surface       | Dst       | NumParams | U        | V        | R       |
|                 | Ai            |           |           |          |          |         |


### CONDITION

- Op.op=sample_b


| | | | | | | |
| --- | --- | --- | --- | --- | --- | --- |
| 0x6d(3D_SAMPLE) | Op           | Exec_size | Pred      | Channels | Aoffimmi | Sampler |
|                 | Surface      | Dst       | NumParams | Bias\*   | U        | V       |
|                 | R            | Ai        |           |          |          |         |


### CONDITION

- Op.op=sample_b_c


| | | | | | | |
| --- | --- | --- | --- | --- | --- | --- |
| 0x6d(3D_SAMPLE) | Op             | Exec_size | Pred      | Channels | Aoffimmi | Sampler |
|                 | Surface        | Dst       | NumParams | Ref\*    | Bias\*   | U       |
|                 | V              | R         | Ai        |          |          |         |


### CONDITION

- Op.op=sample_c, sample_c_lz


| | | | | | | |
| --- | --- | --- | --- | --- | --- | --- |
| 0x6d(3D_SAMPLE) | Op              | Exec_size | Pred      | Channels | Aoffimmi | Sampler |
|                 | Surface         | Dst       | NumParams | Ref\*    | U        | V       |
|                 | R               | Ai        |           |          |          |         |


### CONDITION

- Op.op=sample_d


| | | | | | | |
| --- | --- | --- | --- | --- | --- | --- |
| 0x6d(3D_SAMPLE) | Op           | Exec_size | Pred      | Channels | Aoffimmi | Sampler |
|                 | Surface      | Dst       | NumParams | U        | Dudx\*   | Dudy\*  |
|                 | V            | Dvdx\*    | Dvdy\*    | R        | Drdx\*   | Drdy\*  |
|                 | Ai           |           |           |          |          |         |


### CONDITION

- Op.op=sample_d_c


| | | | | | | |
| --- | --- | --- | --- | --- | --- | --- |
| 0x6d(3D_SAMPLE) | Op             | Exec_size | Pred      | Channels | Aoffimmi | Sampler |
|                 | Surface        | Dst       | NumParams | Ref\*    | U        | Dudx\*  |
|                 | Dudy\*         | V         | Dvdx\*    | Dvdy\*   | R        | Drdx\*  |
|                 | Drdy\*         | Ai        |           |          |          |         |


### CONDITION

- Op.op=sample_l


| | | | | | | |
| --- | --- | --- | --- | --- | --- | --- |
| 0x6d(3D_SAMPLE) | Op           | Exec_size | Pred      | Channels | Aoffimmi | Sampler |
|                 | Surface      | Dst       | NumParams | Lod\*    | U        | V       |
|                 | R            | Ai        |           |          |          |         |


### CONDITION

- Op.op=sample_l_c


| | | | | | | |
| --- | --- | --- | --- | --- | --- | --- |
| 0x6d(3D_SAMPLE) | Op             | Exec_size | Pred      | Channels | Aoffimmi | Sampler |
|                 | Surface        | Dst       | NumParams | Ref\*    | Lod\*    | U       |
|                 | V              | R         | Ai        |          |          |         |


## Semantics




      Sample a surface

## Description



    Samples <surface> using the indicated sampler state <sampler>. LOD, bias, ref, and gradients are computed differently based on the sampler operation. Up to 4 channels of data is returned for each pixel in <dst>.

- **Op(ub):** 
 
  - Bit[4..0]: encodes the sampler operation
 
    - 0b00000:  sample 
    - 0b00001:  sample_b 
    - 0b00010:  sample_l 
    - 0b00011:  sample_c 
    - 0b00100:  sample_d 
    - 0b00101:  sample_b_c 
    - 0b00110:  sample_l_c 
    - 0b01001:  LOD 
    - 0b10100:  sample_d_c 
    - 0b11000:  sample_lz 
    - 0b11001:  sample_c_lz 
  - Bit[5]: pixel null mask enable. Specifies whether the writeback message will include an extra phase indicating the pixel null mask.
 
  - {ICLLP+}Bit[6]: CPS LOD compensation enable. Only available for sample, sample_b, sample_bc, sample_c, and LOD.
 
  - Bit[7]: non-uniform sampler state. true if the sampler field may be different for each thread

- **Exec_size(ub):** Execution size
 
  - Bit[2..0]: size of the region for source and destination operands
 
    - 0b011:  8 elements 
    - 0b100:  16 elements 
  - Bit[7..4]: execution mask (explicit control over the enabled channels)
 
    - 0b0000:  M1 
    - 0b0001:  M2 
    - 0b0010:  M3 
    - 0b0011:  M4 
    - 0b0100:  M5 
    - 0b0101:  M6 
    - 0b0110:  M7 
    - 0b0111:  M8 
    - 0b1000:  M1_NM 
    - 0b1001:  M2_NM 
    - 0b1010:  M3_NM 
    - 0b1011:  M4_NM 
    - 0b1100:  M5_NM 
    - 0b1101:  M6_NM 
    - 0b1110:  M7_NM 
    - 0b1111:  M8_NM
- **Pred(uw):** Predication control

- **Channels(ub):** 
 
  - Bit[3..0]: determines the write masks for the RGBA channel, with R being bit 0 and A bit 3. At least one channel must be enabled (i.e., "0000" is not allowed)

- **Aoffimmi(scalar):**  A UW representing the _aoffimmi modifier with the following format:
 
  - Bit[3..0]: stores the R offset. Valid values are [-8-7]
 
  - Bit[7..4]: stores the V offset. Valid values are [-8-7]
 
  - Bit[11..8]: stores the U offset. Valid values are [-8-7]
 
  - Bit[15..12]: reserved. Must be zero

- **Sampler(ub):** Index of the sampler variable

- **Surface(ub):** Index of the surface variable

- **Dst(raw_operand):** The result of the sample. Must have type HF,F,W,UW,D,UD

- **NumParams(ub):** number of additional parameters for this instruction. Valid values are  [1-15]

- **Ref(raw_operand):** The first <exec_size> elements contain the reference value that is compared against the red channel of the sampled surface

- **Bias(raw_operand):** The first <exec_size> elements contain the LOD bias, with range of [-16.0, 16.0]

- **Lod(raw_operand):** The first <exec_size> elements contain the LOD

- **U(raw_operand):** The first <exec_size> elements contain the normalized x coordinates

- **Dudx(raw_operand):** The first <exec_size> elements contain the gradients of the u coordinate

- **Dudy(raw_operand):** The first <exec_size> elements contain the gradients of the u  coordinate

- **V(raw_operand):** The first <exec_size> elements contain for

                -  1D_array surfaces: unnormalized array index
                -  Other surface types: normalized y coordinate
      - **Dvdx(raw_operand):** The first <exec_size> elements contain the gradients of the v coordinate

- **Dvdy(raw_operand):** The first <exec_size> elements contain the gradients of the v coordinate

- **R(raw_operand):** The first <exec_size> elements contain for

                -  2D_array: the unnormalized array index.
                -  Other surface types: the normalized z coordinates
      - **Drdx(raw_operand):** The first <exec_size> elements contain the gradients of the r coordinate

- **Drdy(raw_operand):** The first <exec_size> elements contain the gradients of the v  coordinate

- **Ai(raw_operand):** The first <exec_size> elements contain the array index for a cube surface

#### Properties


## Text
```
    



[(<P>)] SAMPLE_3d[.pixel_null_mask][.cps][.divS][.divS].<Channels> (Exec_size) <Aoffimmi> <Sampler> <Surface> <Dst> <u> <v> <r> <ai>

[(<P>)] SAMPLE_B[.pixel_null_mask][.cps][.divS].<Channels> (Exec_size) <Aoffimmi> <Sampler> <Surface> <Dst> <bias> <u> <v> <r> <ai>

[(<P>)] SAMPLE_L[.pixel_null_mask][.cps][.divS].<Channels> (Exec_size) <Aoffimmi> <Sampler> <Surface> <Dst> <lod> <u> <v> <r> <ai>

[(<P>)] SAMPLE_C[.pixel_null_mask][.cps][.divS].<Channels> (Exec_size) <Aoffimmi> <Sampler> <Surface> <Dst> <ref> <u> <v> <r> <ai>

[(<P>)] SAMPLE_D[.pixel_null_mask][.cps][.divS].<Channels> (Exec_size) <Aoffimmi> <Sampler> <Surface> <Dst> <u> <dudx> <dudy> <v> <dvdx> <dvdy> <r> <drdx> <drdy> <ai>

[(<P>)] SAMPLE_B_C[.pixel_null_mask][.cps][.divS].<Channels> (Exec_size) <Aoffimmi> <Sampler> <Surface> <Dst> <ref> <bias> <u> <v> <r> <ai>

[(<P>)] SAMPLE_L_C[.pixel_null_mask][.cps][.divS].<Channels> (Exec_size) <Aoffimmi> <Sampler> <Surface> <Dst> <ref> <lod> <u> <v> <r> <ai>

[(<P>)] LOD[.pixel_null_mask][.cps][.divS].<Channels> (Exec_size) <Aoffimmi> <Sampler> <Surface> <Dst> <u> <v> <r> <ai>

[(<P>)] SAMPLE_D_C[.pixel_null_mask][.cps][.divS].<Channels> (Exec_size) <Aoffimmi> <Sampler> <Surface> <Dst> <ref> <u> <dudx> <dudy> <v> <dvdx> <dvdy> <r> <drdx> <drdy> <ai>

[(<P>)] SAMPLE_LZ[.pixel_null_mask][.cps][.divS].<Channels> (Exec_size) <Aoffimmi> <Sampler> <Surface> <Dst> <u> <v> <r> <ai>

[(<P>)] SAMPLE_C_LZ[.pixel_null_mask][.cps][.divS].<Channels> (Exec_size) <Aoffimmi> <Sampler> <Surface> <Dst> <ref> <u> <v> <r> <ai>

// instruction specific parameters may vary
```



## Notes




For each enabled channel <exec_size> elements are returned in RGBA order, with the disabled channels skipped in the results. Only the enabled pixels are returned in <dst>. Each channel's return data start in the next GRF; if <exec_size> * sizeof(dst_type) is smaller than the register size, the remaining portions of the register have undefined values.

For LOD operation: R channel contains the clamped LOD values while G channel contains the unclamped LOD values. B and A channels have undefined values and should be masked out.

For all operations, if <pixel_null_mask> is set, an additional GRF is returned after the sampler data, with <exec_size> bits in the first DWord containing the pixel null mask values. This field has the bit for all pixels set to 1 except those pixels in which a null page was source for at least one texel.

Extra parameters (after NumParams) for this instruction are required only for certain operations and surface types.

  - **{pre-ICLLP}** All operands must have type F.
  - **{ICLLP+}** All operands must have the same type, which can be either HF or F.
  - It is permitted to skip the trailing parameters; the missing parameters will have the value of 0.


The table below summarizes the additional arguments for each of the sampler operations.

    +-----------------+------------------------------------------------------------------------------------------------+
    | Operation       | Parameters                                                                                     |
    |                 +--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    |                 | 0      | 1      | 2      | 3      | 4      | 5      | 6      | 7      | 8      | 9      | 10   |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    | sample          | u      | v      | r      | ai     |        |        |        |        |        |        |      |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    | sample_b        | bias   | u      | v      | r      | ai     |        |        |        |        |        |      |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    | sample_l        | lod    | u      | v      | r      | ai     |        |        |        |        |        |      |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    | sample_c        | ref    | u      | v      | r      | ai     |        |        |        |        |        |      |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    | sample_d        | u      | dudx   | dudy   | v      | dvdx   | dvdy   | r      | drdx   | drdy   | ai     |      |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    | sample_b_c      | ref    | bias   | u      | v      | r      | ai     |        |        |        |        |      |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    | sample_l_c      | ref    | lod    | u      | v      | r      | ai     |        |        |        |        |      |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    | LOD             | u      | v      | r      | ai     |        |        |        |        |        |        |      |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    | sample_d_c      | ref    | u      | dudx   | dudy   | v      | dvdx   | dvdy   | r      | drdx   | drdy   | ai   |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    | sample_lz       | u      | v      | r      | ai     |        |        |        |        |        |        |      |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+
    | sample_c_lz     | ref    | u      | v      | r      | ai     |        |        |        |        |        |      |
    +-----------------+--------+--------+--------+--------+--------+--------+--------+--------+--------+--------+------+

The table below describes the meanings of the u, v, r, ai parameters based on the surface type.

    +----------------+-----------------------------+-----------------------------+-----------------------------+----------------------------+
    | Surface Type   | u                           | v                           | r                           | ai                         |
    +----------------+-----------------------------+-----------------------------+-----------------------------+----------------------------+
    | 1D/1D_ARRAY    | normalized 'x' coordinate   | unnormalized array index    | ignored                     | ignored                    |
    +----------------+-----------------------------+-----------------------------+-----------------------------+----------------------------+
    | 2D/2D_ARRAY    | normalized 'x' coordinate   | normalized 'y' coordinate   | unnormalized array index    | ignored                    |
    +----------------+-----------------------------+-----------------------------+-----------------------------+----------------------------+
    | 3D             | normalized 'x' coordinate   | normalized 'y' coordinate   | normalized 'z' coordinate   | ignored                    |
    +----------------+-----------------------------+-----------------------------+-----------------------------+----------------------------+
    | CUBE           | normalized 'x' coordinate   | normalized 'y' coordinate   | normalized 'z' coordinate   | unnormalized array index   |
    +----------------+-----------------------------+-----------------------------+-----------------------------+----------------------------+
