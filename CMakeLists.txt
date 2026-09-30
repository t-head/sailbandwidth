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

set(REL_NO_SYM FALSE CACHE BOOL "Release with stripped symbols and anonymized paths")
message(STATUS "REL_NO_SYM=${REL_NO_SYM}")

set(CMAKE_C_COMPILER ${SDK_PATH}/bin/hgcc)
set(CMAKE_CXX_COMPILER ${SDK_PATH}/bin/hgcc)

execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -Xcompiler -ffile-prefix-map=/tmp=. -x c /dev/null -c -o /dev/null
    RESULT_VARIABLE PREFIX_MAP_RESULT
    OUTPUT_QUIET ERROR_QUIET
)

if(PREFIX_MAP_RESULT EQUAL 0)
    set(FILE_PREFIX_MAP "-ffile-prefix-map")
else()
    set(FILE_PREFIX_MAP "-fmacro-prefix-map;-fdebug-prefix-map")
endif()
message(STATUS "FILE_PREFIX_MAP=${FILE_PREFIX_MAP}")

set(USE_VPIPEID FALSE CACHE BOOL "Enable USE_VPIPEID compile macro")
message(STATUS "USE_VPIPEID=${USE_VPIPEID}")

set(CMAKE_SKIP_BUILD_RPATH TRUE)

project(sailbandwidth LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(READ /etc/os-release OS_INFO)
string(TOLOWER "${OS_INFO}" OS_INFO_LOWER)
if(OS_INFO_LOWER MATCHES "alios")
    add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
endif()

# CXX=hgcc breaks CMake's multiarch detection; FindBoost can't
# locate libs on Debian. Glob manually.
if(NOT BOOST_LIBRARYDIR)
    file(GLOB_RECURSE _BOOST_PROBE "/usr/*boost*.so" "/usr/*boost*.a")
    if(_BOOST_PROBE)
        list(GET _BOOST_PROBE 0 _FIRST)
        cmake_path(GET _FIRST PARENT_PATH BOOST_LIBRARYDIR)
        message(STATUS "BOOST_LIBRARYDIR=${BOOST_LIBRARYDIR}")
    endif()
endif()

set(Boost_USE_STATIC_LIBS ON)
find_package(Boost COMPONENTS program_options REQUIRED)

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
    src/output.cpp
    src/json_output.cpp
    json/jsoncpp.cpp
)
set_source_files_properties(src/kernels.cu PROPERTIES LANGUAGE CXX)

add_executable(sailbandwidth ${src})

set(HGGC_INC ${SDK_PATH}/include)
set(ALIPPU_LIB "${SDK_PATH}/lib/libalippu.so")
set(ALIPPU_LINK_DIR ${SDK_PATH}/lib)
set(UKI_LIB "${SDK_PATH}/lib/libuki.so")
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

get_filename_component(SAILBANDWIDTH_ROOT ${CMAKE_CURRENT_SOURCE_DIR} ABSOLUTE)
if (REL_NO_SYM)
    foreach(_prefix_map_option IN LISTS FILE_PREFIX_MAP)
        list(APPEND PPU_FLAGS -Xcompiler=${_prefix_map_option}=${SAILBANDWIDTH_ROOT}=.)
    endforeach()
endif()

target_include_directories(sailbandwidth PRIVATE . src src/include ${HGGC_INC})
target_compile_options(sailbandwidth PRIVATE
    ${PPU_FLAGS}
    -Xcompiler=-Wall
    -Xcompiler=-Wvla
    -Xcompiler=-Wno-unused-function
    -Xcompiler=-Wno-sign-compare
    -Xcompiler=-Wno-deprecated-declarations
    -Xcompiler=-Wno-unused-private-field
)

target_link_options(sailbandwidth PRIVATE ${PPU_SPLIT_ARCHS} "-L${ALIPPU_LINK_DIR}")
target_link_libraries(sailbandwidth PRIVATE
    Boost::program_options
    ${ALIPPU_LIB}
    ${UKI_LIB}
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

if (REL_NO_SYM)
    add_custom_command(TARGET sailbandwidth POST_BUILD
        COMMAND strip --strip-all $<TARGET_FILE:sailbandwidth>
    )
endif()
