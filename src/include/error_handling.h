/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * Modifications Copyright (c) 2025-2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ERROR_HANDLING_H_
#define ERROR_HANDLING_H_

#include <cstdarg>

void RecordError(const std::stringstream &errmsg);
void RecordWarning(const std::stringstream &warnmsg);

// inline void format_buffer(char *buffer, const size_t length, const char *fmt, ...) {
//   va_list vargs;
//   va_start(vargs, fmt);
//   (void)vsnprintf(buffer, length, fmt, vargs);
//   va_end(vargs);
// }

#ifdef MULTINODE
#define HOST_INFO " on " << localHostname << ", rank = " << worldRank
#else
#define HOST_INFO ""
#endif

#ifdef MULTINODE
#include <mpi.h>
#define MPI_ABORT MPI_Abort(MPI_COMM_WORLD, 1)
#else
#define MPI_ABORT
#endif

// HGGC Error handling
#define HGGC_ASSERT(x) do { \
    hggcError_t hggcErr = (x); \
    if ((hggcErr) != hggcSuccess) { \
        std::stringstream errmsg; \
        errmsg << "[" << hggcGetErrorName(hggcErr) << "] " << hggcGetErrorString(hggcErr) << " in expression " << #x << HOST_INFO << " in " << __PRETTY_FUNCTION__ << "() : " << __FILE__ << ":" <<  __LINE__ << std::endl; \
        RecordError(errmsg); \
        MPI_ABORT; \
        std::exit(1); \
    }  \
} while ( 0 )

#define HG_ASSERT(x) do { \
    HGresult hgResult = (x); \
    if ((hgResult) != HGGC_SUCCESS) { \
        const char *errDescStr, *errNameStr; \
        hgGetErrorString(hgResult, &errDescStr); \
        hgGetErrorName(hgResult, &errNameStr); \
        std::stringstream errmsg; \
        errmsg << "[" << errNameStr << "] " << errDescStr << " in expression " << #x << HOST_INFO << " in " << __PRETTY_FUNCTION__ << "() : " << __FILE__ << ":" <<  __LINE__ << std::endl; \
        RecordError(errmsg); \
        MPI_ABORT; \
        std::exit(1); \
    }  \
} while ( 0 )

// HGML Error handling
#define HGML_ASSERT(x) do { \
    hgmlReturn_t hgmlResult = (x); \
    if ((hgmlResult) != HGML_SUCCESS) { \
        std::stringstream errmsg; \
        errmsg << "HGML_ERROR: [" << hgmlErrorString(hgmlResult) << "] in expression " << #x << HOST_INFO << " in " << __PRETTY_FUNCTION__ << "() : " << __FILE__ << ":" <<  __LINE__ << std::endl; \
        RecordError(errmsg); \
        MPI_ABORT; \
        std::exit(1); \
    }  \
} while ( 0 )

// Generic Error handling
#define ASSERT(x) do { \
    if (!(x)) { \
        std::stringstream errmsg; \
        errmsg << "ASSERT in expression " << #x << HOST_INFO << " in " << __PRETTY_FUNCTION__ << "() : " << __FILE__ << ":" <<  __LINE__  << std::endl; \
        RecordError(errmsg); \
        MPI_ABORT; \
        std::exit(1); \
    }  \
} while ( 0 )

#define WARN(msg) \
    do { \
        std::stringstream warnmsg; \
        warnmsg << "Warning: " << (msg) << " in expression " << HOST_INFO << " in " << __PRETTY_FUNCTION__ << "() : " << __FILE__ << ":" <<  __LINE__ << std::endl; \
        RecordWarning(warnmsg); \
    } while(0)

#endif  // ERROR_HANDLING_H_
