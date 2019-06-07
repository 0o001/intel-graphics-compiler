# Intel® Graphics Compiler for OpenCL™

## Introduction

The Intel® Graphics Compiler for OpenCL™ is an LLVM based compiler for
OpenCL™ targeting Intel Gen graphics hardware architecture.

Please refer to http://01.org/compute-runtime for additional details regarding
 Intel's motivation and intentions wrt OpenCL support in the open source.


## License

The Intel® Graphics Compute Runtime for OpenCL™ is distributed under the MIT License.

You may obtain a copy of the License at:

https://opensource.org/licenses/MIT

## Dependencies

* LLVM Source -  https://github.com/llvm-mirror/llvm
* Clang Source - https://github.com/llvm-mirror/clang
* OpenCL Clang - https://github.com/intel/opencl-clang
* SPIRV-LLVM Translator - https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git

## Supported Linux versions

IGC is supported on the following 64 bit Linux operating systems:

* Ubuntu 16.04, 18.04, 19.04

## Building

* [Ubuntu](https://github.com/intel/intel-graphics-compiler/master/documentation/build_ubuntu.md)

## Supported Platforms

* Intel Core Processors supporting Gen8 graphics devices
* Intel Core Processors supporting Gen9 graphics devices
* Intel Core Processors supporting Gen11 graphics devices
* Intel Atom Processors supporting Gen9 graphics devices

## How to provide feedback
Please submit an issue using native github.com interface: https://github.com/intel/intel-graphics-compiler/issues.

## How to contribute

Create a pull request on github.com with your patch. Make sure your change is
cleanly building. A maintainer will contact you if there are questions or concerns.
