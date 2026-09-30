/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
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

#ifndef VERIFY_UTILS_H_
#define VERIFY_UTILS_H_

#include <memory>
#include <string>

#include "memcpy.h"

// Data-verification failure reporting and SAILBW_VERIFY fault-injection helpers.

// Link-endpoint id: MPI rank in multinode, else device ordinal.
int verifyLinkId(const MemcpyBuffer* buf);

// SAILBW_VERIFY: corrupt the buffer head if this link is the requested one.
void verifyInjectFaultIfRequested(HGdeviceptr buffer, unsigned long long size, int linkSrcId,
                                  int linkDstId, const std::shared_ptr<NodeHelper>& nodeHelper);

// Report a verification failure (optionally dumped via SAILBW_DUMP_PATH). hostInitPattern may be
// null when the verified buffer's pre-fill pattern is unknown.
void verifyReportFailure(const MemcpyBuffer* srcBuf, const MemcpyBuffer* dstBuf,
                         const MemcpyBuffer* verifiedBuf, const unsigned int* hostPattern,
                         const unsigned int* hostInitPattern, unsigned long long numElements,
                         unsigned int numPatternElements);

#endif  // VERIFY_UTILS_H_
