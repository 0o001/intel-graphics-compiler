#=========================== begin_copyright_notice ============================
#
# Copyright (C) 2021 Intel Corporation
#
# SPDX-License-Identifier: MIT
#
#============================ end_copyright_notice =============================

# Order of chosing way how to take opencl-clang
#1. CCLANG_FROM_SYSTEM - use installed on system opencl-clang toolchain
#2. CCLANG_BUILD_PREBUILDS - use prebuilded opencl-clang toolchain
#   CCLANG_BUILD_PREBUILDS_DIR - set path to prebuilt cclang folder
#3. CCLANG_BUILD_INTREE_LLVM - use sources of opencl-clang toolchain

if(NOT DEFINED COMMON_CLANG_LIBRARY_NAME)
  set(COMMON_CLANG_LIBRARY_NAME opencl-clang)
endif()

set(COMMON_CLANG_LIB_FULL_NAME "lib${COMMON_CLANG_LIBRARY_NAME}${CMAKE_SHARED_LIBRARY_SUFFIX}")

find_library(CCLANG_FROM_SYSTEM ${COMMON_CLANG_LIBRARY_NAME})

### Check if user manual setup some of flag
if(NOT CCLANG_BUILD_PREBUILDS)
  set(CCLANG_BUILD_PREBUILDS FALSE)
else()
  set(CCLANG_FROM_SYSTEM FALSE)
  set(CCLANG_BUILD_INTREE_LLVM FALSE)
endif()

if(NOT CCLANG_BUILD_INTREE_LLVM)
  set(CCLANG_BUILD_INTREE_LLVM FALSE)
else()
  set(CCLANG_BUILD_PREBUILDS FALSE)
  set(CCLANG_FROM_SYSTEM FALSE)
endif()
###

### Check by order first available way to link with opencl-clang
if(NOT CCLANG_FROM_SYSTEM)
  if(NOT CCLANG_BUILD_PREBUILDS_DIR)
    # Detect CPU architecture
    igc_arch_get_cpu(cpuSuffix)
    set(CCLANG_BUILD_PREBUILDS_DIR "${IGC_BUILD__GFX_DEV_SRC_DIR}/../../prebuild-opencl-clang/Release/${cpuSuffix}")

    if(NOT EXISTS ${CCLANG_BUILD_PREBUILDS_DIR})
      set(CCLANG_BUILD_PREBUILDS_DIR "${IGC_BUILD__GFX_DEV_SRC_DIR}/../prebuild-opencl-clang/Release/${cpuSuffix}")
    endif()
  endif()
  set(CCLANG_BUILD_INTREE_LLVM_DIR ${LLVM_SOURCE_DIR}/projects/opencl-clang)
  ### Check if user by choosing some way of linking with opencl-clang provided required folders
  if(${CCLANG_BUILD_PREBUILDS})
    if(NOT EXISTS ${CCLANG_BUILD_PREBUILDS_DIR})
      message(FATAL_ERROR "[IGC] : User setup to use prebuilded opencl-clang but not found folder : ${CCLANG_BUILD_PREBUILDS_DIR}")
      set(CCLANG_BUILD_PREBUILDS FALSE)
    endif()
  elseif(${CCLANG_BUILD_INTREE_LLVM})
    if(NOT EXISTS ${CCLANG_BUILD_INTREE_LLVM_DIR})
      message(FATAL_ERROR "[IGC] : User setup to use sources of opencl-clang but not found folder : ${CCLANG_BUILD_INTREE_LLVM_DIR}")
      set(CCLANG_BUILD_INTREE_LLVM FALSE)
    endif()
  ###
  ### User didn't define nothing, then pick the method 2 or 3.
  elseif(EXISTS ${CCLANG_BUILD_PREBUILDS_DIR})
    set(CCLANG_BUILD_PREBUILDS TRUE)
  elseif(EXISTS ${CCLANG_BUILD_INTREE_LLVM_DIR})
    message(STATUS "[IGC] : opencl-clang will be taken from sources")
    set(CCLANG_BUILD_INTREE_LLVM TRUE)
  else()
    message(FATAL_ERROR "[IGC] : Cannot find opencl-clang tool-chain, please provide sources or install it on system.")
  endif()
  ###

endif()
###

#1. CCLANG_FROM_SYSTEM - use installed on system opencl-clang toolchain
if(CCLANG_FROM_SYSTEM)
  message(STATUS "[IGC] : opencl-clang will be taken from system")

  find_library(SYSTEM_COMMON_CLANG ${COMMON_CLANG_LIBRARY_NAME})

  add_library(opencl-clang-lib SHARED IMPORTED GLOBAL)
  set_property(TARGET opencl-clang-lib PROPERTY "IMPORTED_LOCATION" "${SYSTEM_COMMON_CLANG}")
  find_program(CLANG_GE7 clang-${LLVM_VERSION_MAJOR})
  if(CLANG_GE7)
    message(STATUS "[IGC] Found clang-${LLVM_VERSION_MAJOR} executable: ${CLANG_GE7}")

    add_executable(clang-tool IMPORTED GLOBAL)
    set_property(TARGET clang-tool PROPERTY "IMPORTED_LOCATION" "${CLANG_GE7}")
    set(CL_OPTIONS "-finclude-default-header")
    if(LLVM_VERSION_MAJOR VERSION_EQUAL 7)
      message(WARNING "[IGC] : clang-7 should be patched with VME patch (https://reviews.llvm.org/D51484). Assuming that it is. If not, please add -DVME_TYPES_DEFINED=FALSE.")
    endif()
  else(CLANG_GE7)
    message(FATAL_ERROR "[IGC] : Couldn't find clang-${LLVM_VERSION_MAJOR} executable, please install it.")
  endif(CLANG_GE7)
###
#2. CCLANG_BUILD_PREBUILDS - use prebuilded opencl-clang toolchain
elseif(${CCLANG_BUILD_PREBUILDS})
  message(STATUS "[IGC] : opencl-clang will be taken from prebuilds")

  set(CLANG_TOOL_PATH "${CCLANG_BUILD_PREBUILDS_DIR}/clang${CMAKE_EXECUTABLE_SUFFIX}")
  set(LLVM_PACKAGE_VERSION "${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}${LLVM_VERSION_SUFFIX}")

  if(CMAKE_CROSSCOMPILING)
    # In case of cross compilation we could not execute prebuilt opencl library,
    # so we trust version number provided externally or assume it's the same as
    # LLVM version
    if(NOT DEFINED CLANG_TOOL_VERSION)
      set(CLANG_TOOL_VERSION "${LLVM_PACKAGE_VERSION}")
    endif()
  else(CMAKE_CROSSCOMPILING)
    # Get clang-tool version
    execute_process(
      COMMAND ${CLANG_TOOL_PATH} -v
      ERROR_VARIABLE CLANG_TOOL_V_CALL)
    string(REGEX MATCH "clang version ([0-9]*\\.[0-9]*\\.[0-9]*[a-zA-Z0-9]*)" CLANG_TOOL_VERSION "${CLANG_TOOL_V_CALL}")
    set(CLANG_TOOL_VERSION "${CMAKE_MATCH_1}")

    # Check if we parse clang tool version correctly
    if(NOT CLANG_TOOL_VERSION)
      message(FATAL_ERROR "[IGC] : Cannot read version of clang tool, please check the output of execution `clang -v` : ${CLANG_TOOL_V_CALL}")
    endif()
  endif(CMAKE_CROSSCOMPILING)

  # Check if llvm version for IGC is newer or equal with the clang-tool version
  if(${LLVM_PACKAGE_VERSION} VERSION_GREATER ${CLANG_TOOL_VERSION} OR
     ${LLVM_PACKAGE_VERSION} EQUAL ${CLANG_TOOL_VERSION})
    add_library(opencl-clang-lib SHARED IMPORTED GLOBAL)
    set_property(TARGET opencl-clang-lib PROPERTY "IMPORTED_LOCATION" "${CCLANG_BUILD_PREBUILDS_DIR}/${COMMON_CLANG_LIB_FULL_NAME}")

    add_executable(clang-tool IMPORTED GLOBAL)
    set_property(TARGET clang-tool PROPERTY "IMPORTED_LOCATION" "${CLANG_TOOL_PATH}")

    set(opencl-header "${CCLANG_BUILD_PREBUILDS_DIR}/opencl-c.h")
  else()
    message(FATAL_ERROR "[IGC] : The clang-tool(${CLANG_TOOL_VERSION}) from prebuilts is newer than llvm(${LLVM_PACKAGE_VERSION}) version for IGC.")
  endif()
###
#3. CCLANG_BUILD_INTREE_LLVM - use sources of opencl-clang toolchain
elseif(${CCLANG_BUILD_INTREE_LLVM})
  message(STATUS "[IGC] : opencl-clang will be taken from sources")

  add_library(opencl-clang-lib ALIAS ${COMMON_CLANG_LIBRARY_NAME})
  add_executable(clang-tool ALIAS clang)
  get_target_property(CLANG_SOURCE_DIR clang SOURCE_DIR)
  set(opencl-header "${CLANG_SOURCE_DIR}/../../lib/Headers/opencl-c.h")
endif()
###
