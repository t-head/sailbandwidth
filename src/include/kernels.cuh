/*
 * SPDX-FileCopyrightText: Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#ifndef KERNELS_CUH_
#define KERNELS_CUH_

#include "memcpy.h"
#include "ppu_common.h"

const unsigned long long DEFAULT_SPIN_KERNEL_TIMEOUT_MS = 10000ULL;   // 10 seconds

size_t copyKernel(MemcpyDescriptor &desc);
size_t copyKernelSplitWarp(MemcpyDescriptor &desc);
size_t multicastCopy(HGdeviceptr dstBuffer, HGdeviceptr srcBuffer, size_t size, HGstream stream, unsigned long long loopCount);
HGresult spinKernel(volatile int *latch, HGstream mainstream, HGstream die0stream, HGstream die1stream, unsigned long long timeoutMs = DEFAULT_SPIN_KERNEL_TIMEOUT_MS);
HGresult spinKernelMultistage(volatile int *latch1, volatile int *latch2, HGstream mainstream, HGstream die0stream, HGstream die1stream, unsigned long long timeoutMs = DEFAULT_SPIN_KERNEL_TIMEOUT_MS);
void preloadKernels(int deviceCount);
double latencyPtrChaseKernel(const int srcId, void* data, size_t size, unsigned long long latencyMemAccessCnt, unsigned smCount);
HGresult memsetKernel(HGstream stream, HGdeviceptr buffer, HGdeviceptr pattern, unsigned long long num_elements, unsigned int num_pattern_elements);
HGresult memcmpKernel(HGstream stream, HGdeviceptr buffer, HGdeviceptr pattern, unsigned long long num_elements, unsigned int num_pattern_elements, HGdeviceptr errorFlag);
HGresult multicastMemcmpKernel(HGstream stream, HGdeviceptr buffer, HGdeviceptr pattern, unsigned long long num_elements, unsigned int num_pattern_elements, HGdeviceptr errorFlag);

HGresult memclearKernelByWarpParity(HGstream stream, HGdeviceptr buffer, size_t size, bool clearOddWarpIndexed);
#endif  // KERNELS_CUH_
