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

#ifndef PPU_COMMON_H_
#define PPU_COMMON_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <float.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <limits.h>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <hggc.h>
#include <hggc_runtime.h>
#include <hgml.h>

#include "common.h"
#include "drv_eit/hggc_eit_pccl.h"
#include "error_handling.h"

// ICN link
#define MAX_ICNLINK_PPU0010 7
#define MAX_ICNLINK_PPU0015 8

#define PER_DIE_ICNLINK_PPU0015 4
// Dma ring
#define MAX_DMA_RING_PPU0015 16

// icn link define
#define PPU_DIE() ((sm < 89)  ? 1 : 2)

#define PPU_ICN_LINK() \
    ((sm < 89)  ? MAX_ICNLINK_PPU0010 : MAX_ICNLINK_PPU0015)

#define PPU_DMA_RING() MAX_DMA_RING_PPU0015

#define PPU_ICN_LINK_PER_DIE() PER_DIE_ICNLINK_PPU0015

#define PPU_DIE0_DMA_RING_ID_BASE 0
#define PPU_DIE1_DMA_RING_ID_BASE (PPU_DMA_RING() / 2)

#define GET_ICN_LINK_NUMBER() \
    ((vpipeIdForDie0 >= 0) ? (vpipeIdForDie0) : \
     (vpipeIdForDie1 >= 0) ? (vpipeIdForDie1 + (PPU_ICN_LINK_PER_DIE())) : \
                             (linkId))

#define ADJUST_SM_COUNT_FOR_SINGLE_DIE(nBlocks) \
    do { if (sm >= 89 && isSingleDie) (nBlocks) = ((nBlocks) / 2 > 0) ? (nBlocks) / 2 : 1; } while(0)


extern bool ppuVpipeIdEnable;

enum class sailbandwidthIcnRemoteType {
    Unknown = 0,
    DirectPPU,
    Switch,
};

typedef struct IcnReadConfig {
   bool enableVpipeIdReadOptimize = false;
   bool enableSplitSizeReadOptimize = false;

   bool useSingleLinkForIcnRead = false;
   uint32_t vpipeId = 0;
   int vpipeIdForDie0 = -1;
   int vpipeIdForDie1 = -1;
} IcnReadConfig;

// vpipe id class
class EitTableProvider {
 public:
    static HGresult hgMemCreateEx(HGmemGenericAllocationHandle* handle, size_t size, const HGmemAllocationProp* prop, unsigned long long flags);
    static HGresult setStreamToVpipeId(HGstream stream, HGdeviceptr buffer, uint32_t vpipeId);
    static HGresult setStreamToRingId(HGstream stream, uint32_t ringId);

 private:
    EitTableProvider();

    static const EitTableProvider& getInstance();

    const HGeitPccl* eitTable = nullptr;
    hggcError_t eitTableResult = hggcErrorInvalidValue;
    hggcError_t copyPrimitiveResult = hggcErrorInvalidValue;
    hggcError_t streamHintResult = hggcErrorInvalidValue;
};

int getDeviceCompCap(int hggcDev = -1);
void setCUMask(int dev, const void* func);
bool getIcnTotalBW(int srcDev, int dstDev, std::string key, int *icnTotalBW, bool isMultiTest);

sailbandwidthIcnRemoteType detectIcnRemoteType(unsigned int deviceIndex);

#endif  // PPU_COMMON_H_
