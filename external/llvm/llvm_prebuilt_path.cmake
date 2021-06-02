#=========================== begin_copyright_notice ============================
#
# Copyright (C) 2021 Intel Corporation
#
# SPDX-License-Identifier: MIT
#
#============================ end_copyright_notice =============================

set(LLVM_BUILD_TYPE ${CMAKE_BUILD_TYPE})

if(DEFINED BUILD_TYPE)
  if(${BUILD_TYPE} STREQUAL "release")
    set(LLVM_BUILD_TYPE "Release")
  else()
    set(LLVM_BUILD_TYPE "Debug")
  endif()
endif()

list(APPEND DEFAULT_IGC_LLVM_PREBUILDS_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../prebuild-llvm/${LLVM_BUILD_TYPE}")
