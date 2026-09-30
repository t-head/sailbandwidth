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

#include <cstddef>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

#include "ppu_common.h"

#ifdef USE_VPIPEID
bool ppuVpipeIdEnable = true;
#else
bool ppuVpipeIdEnable = false;
#endif

// ppu common helpers
namespace {
struct ppuDispatchMask_105 {
    uint64_t cu_mask;
    uint64_t cu_mask1;
    uint64_t cu_mask2;
};

template<typename TableType>
const TableType* getExportTableByUUID(const hggcUUID_t* uuid) {
    const TableType* table = nullptr;
    HGGC_ASSERT(hggcGetExportTable(
        reinterpret_cast<const void**>(&table),
        uuid
    ));
    return table;
}
}  // namespace

int getDeviceCompCap(int hggcDev) {
    if (hggcDev == -1) {
        if (hggcGetDevice(&hggcDev) != hggcSuccess) return 0;
    }
    int ccMajor, ccMinor;
    if (hggcDeviceGetAttribute(&ccMajor, hggcDevAttrComputeCapabilityMajor, hggcDev) != hggcSuccess) return 0;
    if (hggcDeviceGetAttribute(&ccMinor, hggcDevAttrComputeCapabilityMinor, hggcDev) != hggcSuccess) return 0;
    return ccMajor * 10 + ccMinor;
}

void setCUMask(int dev, const void* func) {
    if (sm == 89) {
        static std::unordered_map<int, std::unordered_set<const void*>> processed_funcs;
        if (processed_funcs[dev].count(func)) {
            return;
        }

        constexpr uint64_t kCUDispatchMaskL = 0x00000000000FFFFF;
        constexpr uint64_t kCUDispatchMaskH = 0x000FFFFF00000000;
        ppuDispatchMask_105 cuMask = {0};
        if (isSingleDie) {
            cuMask.cu_mask = kCUDispatchMaskL;
        } else {
            cuMask.cu_mask = kCUDispatchMaskH | kCUDispatchMaskL;
        }

        static const auto* pFuncTable = getExportTableByUUID<HGeitPccl>(
            reinterpret_cast<const hggcUUID_t*>(&HG_EIT_ID_PCCL)
        );

        HGfunction pFunc = NULL;
        HGGC_ASSERT(hggcGetFuncBySymbol(&pFunc, func));
        HGGC_ASSERT(static_cast<hggcError_t>(pFuncTable->hgEitFuncSetAttribute(pFunc,
                                           HG_EIT_FUNC_ATTRIBUTE_DISPATCH_CU_MASK, &cuMask, sizeof(cuMask))));
        processed_funcs[dev].insert(func);
    }
}

bool getIcnTotalBW(int srcDev, int dstDev, std::string key, int *icnTotalBW, bool isMultiTest) {
    unsigned int icnSpeed = 50;
    const int nDies = (sm < 89 || isSingleDie) ? 1 : 2;
    int minPath = nDies;
    const int minPathEnum = 256;
    const int maxIcnLinks = PPU_ICN_LINK();

    hgmlDevice_t srcHandle;
    if (hgmlDeviceGetHandleByIndex((isMultiTest ? 0 : srcDev), &srcHandle) != HGML_SUCCESS) {
        WARN("Failed to get device handle.");
        return false;
    } else {
        hgmlFieldValue_t values;
        int valuesCount = 1;

        values.fieldId = HGML_FI_DEV_ICNLINK_SPEED_MBPS_L0;
        hgmlDeviceGetFieldValues(srcHandle, valuesCount, &values);
        if (values.hgmlReturn == HGML_SUCCESS) {
            icnSpeed = values.value.uiVal / 1024;
        } else {
            // output->recordWarning("Failed to get field values. The icnlink speed is set as 50 GB/s by default.");
            WARN("Failed to get field values. The icnlink speed is set as 50 GB/s by default.");
        }
    }

    if (key.find("read_ce") == std::string::npos || disableVm) {
        if (isMultiTest) {
            int activeLinkNum = 0;
            for (int curLink = 0; curLink < maxIcnLinks; curLink++) {
                hgmlEnableState_t isActive;
                hgmlDeviceGetIcnLinkState(srcHandle, curLink, &isActive);
                if (isActive == HGML_FEATURE_ENABLED) activeLinkNum++;
            }
            minPath = activeLinkNum > 0 ? activeLinkNum : nDies;
        } else {
            int singleDieMinPath = 1;
            if (hggcDeviceGetP2PAttribute(&singleDieMinPath, (hggcDeviceP2PAttr)(minPathEnum), srcDev, dstDev) != hggcSuccess) {
                // output->recordWarning("Failed to get singleDieMinPath. The icnlink number is set as 1 by default.");
                WARN("Failed to get singleDieMinPath. The icnlink number is set as 1 by default.");
                singleDieMinPath = 1;
            }
            minPath = singleDieMinPath * nDies;
        }
    }

    *icnTotalBW = minPath * icnSpeed;
    if (key.find("bidirectional") != std::string::npos) *icnTotalBW *= 2;

    return true;
}

// get icn remote link
sailbandwidthIcnRemoteType detectIcnRemoteType(unsigned int deviceIndex) {
    sailbandwidthIcnRemoteType result = sailbandwidthIcnRemoteType::Unknown;
    hgmlReturn_t rc = hgmlInit_v2();
    if (rc != HGML_SUCCESS) {
        fprintf(stderr, "hgmlInit_v2 failed: %s\n", hgmlErrorString(rc));
        return result;
    }
    hgmlDevice_t device = nullptr;
    rc = hgmlDeviceGetHandleByIndex(deviceIndex, &device);
    if (rc != HGML_SUCCESS) {
        fprintf(stderr, "hgmlDeviceGetHandleByIndex(%u) failed: %s\n", deviceIndex, hgmlErrorString(rc));
        hgmlShutdown();
        return result;
    }
    for (unsigned int link = 0; link < HGML_ICNLINK_MAX_LINKS; link++) {
        hgmlIntIcnLinkDeviceType_t remoteType = HGML_ICNLINK_DEVICE_TYPE_UNKNOWN;
        rc = hgmlDeviceGetIcnLinkRemoteDeviceType(device, link, &remoteType);
        if (rc != HGML_SUCCESS) {
            continue;
        }
        if (remoteType == HGML_ICNLINK_DEVICE_TYPE_SWITCH) {
            result = sailbandwidthIcnRemoteType::Switch;
            break;
        } else if (remoteType == HGML_ICNLINK_DEVICE_TYPE_PPU) {
            result = sailbandwidthIcnRemoteType::DirectPPU;
            break;
        }
    }
    
    hgmlShutdown();
    return result;
}

// eit table class
namespace {
template <typename FieldType>
bool hasEitField(const HGeitPccl* eit, size_t fieldOffset, FieldType HGeitPccl::*field) {
    return eit != nullptr &&
           eit->struct_size >= fieldOffset + sizeof(FieldType) &&
           eit->*field != nullptr;
}

#define HAS_EIT_FIELD(eit, field) \
    hasEitField((eit), offsetof(HGeitPccl, field), &HGeitPccl::field)

}  // namespace

EitTableProvider::EitTableProvider() {
    eitTableResult = hggcGetExportTable(reinterpret_cast<const void**>(&eitTable), reinterpret_cast<const hggcUUID_t*>(&HG_EIT_ID_PCCL));
    if (eitTableResult != hggcSuccess || eitTable == nullptr) {
        eitTable = nullptr;
        if (eitTableResult == hggcSuccess) {
            eitTableResult = hggcErrorInvalidValue;
        }
        return;
    }

    copyPrimitiveResult = (HAS_EIT_FIELD(eitTable, hgMemCreateEx) && HAS_EIT_FIELD(eitTable, hgStreamSetAttribute)) ? hggcSuccess : hggcErrorInvalidValue;
    streamHintResult = HAS_EIT_FIELD(eitTable, hgStreamHintChannel) ? hggcSuccess : hggcErrorInvalidValue;
}

const EitTableProvider& EitTableProvider::getInstance() {
    static const EitTableProvider provider;
    return provider;
}

HGresult EitTableProvider::hgMemCreateEx(HGmemGenericAllocationHandle* handle, size_t size, const HGmemAllocationProp* prop, unsigned long long flags) {
    const EitTableProvider& provider = getInstance();
    HGGC_ASSERT(provider.copyPrimitiveResult);
    return provider.eitTable->hgMemCreateEx(handle, size, prop, flags);
}

HGresult EitTableProvider::setStreamToVpipeId(HGstream stream, HGdeviceptr buffer, uint32_t vpipeId) {
    if (stream == nullptr || static_cast<uint32_t>(vpipeId) >= PPU_ICN_LINK()) {
        return HGGC_ERROR_INVALID_VALUE;
    }

    vpipeId = vpipeId % 4;

    const EitTableProvider& provider = getInstance();
    if (provider.copyPrimitiveResult != hggcSuccess) {
        return HGGC_ERROR_INVALID_VALUE;
    }
    const HGeitPccl* eit = provider.eitTable;
    
    HGdeviceptr cpdptr = buffer + static_cast<uint32_t>(vpipeId) * sizeof(uint32_t);
    HGeitStreamAttrValue value = {};
    value.cdpPtr = cpdptr;

    HGresult result = hgMemsetD8(cpdptr, 0, sizeof(uint32_t));
    if (result != HGGC_SUCCESS) {
        return result;
    }
    result = eit->hgStreamSetAttribute(stream, HG_EIT_STREAM_ATTRIBUTE_MEMCPY_PRIMITIVE, &value);
    if (result != HGGC_SUCCESS) {
        return result;
    }
    return hgStreamSynchronize(stream);
}

HGresult EitTableProvider::setStreamToRingId(HGstream stream, uint32_t ringId) {
    if (stream == nullptr || ringId >= PPU_DMA_RING()) {
        return HGGC_ERROR_INVALID_VALUE;
    }

    const EitTableProvider& provider = getInstance();
    if (provider.streamHintResult != hggcSuccess) {
        return HGGC_ERROR_INVALID_VALUE;
    }
    const HGeitPccl* eit = provider.eitTable;
    HGresult result = eit->hgStreamHintChannel(stream, HG_STREAM_CHANNEL_PEER, ringId);
    if (result != HGGC_SUCCESS) {
        return result;
    }
    return hgStreamSynchronize(stream);
}
