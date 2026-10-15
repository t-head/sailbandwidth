#########################################################################
# SPDX-FileCopyrightText: Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# Modifications Copyright (c) 2025-2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#########################################################################

cmake_minimum_required(VERSION 3.20)

set(SDK_PATH $ENV{PPU_SDK})
message(STATUS "SDK_PATH=${SDK_PATH}")

set(PPU_ARCHS "ppu_10,ppu_15" CACHE STRING "PPU ARCH version")
message(STATUS "PPU_ARCHS=${PPU_ARCHS}")

set(CMAKE_C_COMPILER ${SDK_PATH}/bin/hgcc)
set(CMAKE_CXX_COMPILER ${SDK_PATH}/bin/hgcc)

set(USE_VPIPEID FALSE CACHE BOOL "Enable USE_VPIPEID compile macro")
message(STATUS "USE_VPIPEID=${USE_VPIPEID}")

set(CMAKE_SKIP_BUILD_RPATH TRUE)

project(sailbandwidth
    VERSION 2.2.1
    LANGUAGES C CXX)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/sailbandwidth_version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/sailbandwidth_version.h
    @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(READ /etc/os-release OS_INFO)
string(TOLOWER "${OS_INFO}" OS_INFO_LOWER)
if(OS_INFO_LOWER MATCHES "alios")
    add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
endif()

set(src
    src/testcase.cpp
    src/testcases_ce.cpp
    src/testcases_sm.cpp
    src/kernels.cu
    src/memcpy.cpp
    src/verify_utils.cpp
    src/ppu_common.cpp
    src/sailbandwidth.cpp
    src/multinode_memcpy.cpp
    src/multinode_testcases.cpp
    src/environment.cpp
    src/hggc_version_check.cpp
    src/output.cpp
    src/json_output.cpp
    src/perf_parser_output.cpp
    json/jsoncpp.cpp
)
set_source_files_properties(src/kernels.cu PROPERTIES LANGUAGE CXX)

execute_process(
    COMMAND git describe --always --tags
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DGIT_VERSION=\\\"\"${GIT_VERSION}\"\\\"")

add_executable(sailbandwidth ${src})

set(HGGC_INC ${SDK_PATH}/include)
set(HGGC_LIB hg_wrapper)
set(HGGCRT_LIB hggc_wrapper)
find_library(HGML_LIB hgml PATHS ${SDK_PATH}/lib NO_DEFAULT_PATH REQUIRED)

string(REPLACE "," ";" SPLIT_ARCHS ${PPU_ARCHS})
set(PPU_SPLIT_ARCHS)
foreach(_arch IN LISTS SPLIT_ARCHS)
  list(APPEND PPU_SPLIT_ARCHS -arch=${_arch})
endforeach()

set(PPU_FLAGS
    -x hg -rdc true -Xcompiler -fPIC
    ${PPU_SPLIT_ARCHS}
)

if (BUILD_DEBUG)
  list(APPEND PPU_FLAGS -O0 -g -G -dopt 0)
endif()

if (USE_VPIPEID)
  list(APPEND PPU_FLAGS -DUSE_VPIPEID)
endif()

list(APPEND PPU_FLAGS -I${CMAKE_CURRENT_SOURCE_DIR}/src/include)
list(APPEND PPU_FLAGS -I${HGGC_INC})

target_include_directories(sailbandwidth PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    . src src/include ${HGGC_INC})
target_compile_options(sailbandwidth PRIVATE
    ${PPU_FLAGS}
    -Xcompiler=-Wall
    -Xcompiler=-Wvla
    -Xcompiler=-Wno-unused-function
    -Xcompiler=-Wno-sign-compare
    -Xcompiler=-Wno-deprecated-declarations
    -Xcompiler=-Wno-unused-private-field
)

target_link_options(sailbandwidth PRIVATE ${PPU_SPLIT_ARCHS})
target_link_libraries(sailbandwidth PRIVATE
    ${HGGC_LIB}
    ${HGGCRT_LIB}
    ${HGML_LIB}
)

if (MULTINODE)
    # hgcc is not fully compatible with CMake; link MPI manually instead of find_package(MPI)
    execute_process(COMMAND mpicc --showme:incdirs
            OUTPUT_VARIABLE MPI_INC_DIRS OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND mpicc --showme:libdirs
            OUTPUT_VARIABLE MPI_LIB_DIRS OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND mpicc --showme:libs
            OUTPUT_VARIABLE MPI_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
    separate_arguments(MPI_INC_DIRS)
    separate_arguments(MPI_LIB_DIRS)
    separate_arguments(MPI_LIBS)

    target_include_directories(sailbandwidth PRIVATE ${MPI_INC_DIRS})
    target_link_directories(sailbandwidth PRIVATE ${MPI_LIB_DIRS})
    target_link_libraries(sailbandwidth PRIVATE ${MPI_LIBS} pthread)
    target_compile_definitions(sailbandwidth PRIVATE
            OMPI_SKIP_MPICXX
            MULTINODE
    )
endif()
