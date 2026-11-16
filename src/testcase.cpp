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

#include "ppu_common.h"
#include "output.h"
#include "testcase.h"

std::map<std::string, std::map<std::string, std::shared_ptr<PeerValueMatrix<double>>>> Testcase::globalTestCache;

Testcase::Testcase(std::string key, std::string desc) :
    key(std::move(key)), desc(std::move(desc))
{}

std::string Testcase::testKey() { return key; }
std::string Testcase::testDesc() { return desc; }

FilterResult Testcase::filterHasAccessiblePeerPairs() {
    int deviceCount = 0;
    HG_ASSERT(hgDeviceGetCount(&deviceCount));

    for (int currentDevice = 0; currentDevice < deviceCount; currentDevice++) {
        for (int peer = 0; peer < deviceCount; peer++) {
            int canAccessPeer = 0;

            if (peer == currentDevice) {
                continue;
            }

            HG_ASSERT(hgDeviceCanAccessPeer(&canAccessPeer, currentDevice, peer));
            if (canAccessPeer) {
                return FilterResult::pass();
            }
        }
    }

    return FilterResult::waive("no accessible peer PPU pairs");
}

FilterResult Testcase::filterSupportsMulticast() {
    if (worldRank == 0) output->recordWarning("multicast is not supported on PPU");
    return FilterResult::waive("multicast is not supported on PPU");
}

FilterResult Testcase::filterSupportsTMA() {
    return FilterResult::waive("TMA is not implemented on PPU");
}

#ifdef MULTINODE
// Each MPI rank handles one PPU, so we simply have to check if we have more than 1 process
FilterResult Testcase::filterHasMultiplePPUsMultinode() {
    return worldSize > 1 ? FilterResult::pass() : FilterResult::waive("requires at least 2 MPI ranks");
}
#endif

template<typename AlignedLatencyNode>
void Testcase::latencyHelper(const MemcpyBuffer &dataBuffer, bool measureDeviceToDeviceLatency) {
    uint64_t n_ptrs = dataBuffer.getBufferSize() / sizeof(AlignedLatencyNode);

    if (measureDeviceToDeviceLatency) {
        // For device-to-device latency, create and initialize pattern on device
        for (uint64_t i = 0; i < n_ptrs; i++) {
            AlignedLatencyNode alignedNode;
            size_t nextOffset = ((i + strideLen) % n_ptrs) * sizeof(AlignedLatencyNode);
            // Set up pattern with device addresses
            alignedNode.next = (AlignedLatencyNode *)(dataBuffer.getBuffer() + nextOffset);
            HG_ASSERT(hgMemcpyHtoD(dataBuffer.getBuffer() + i*sizeof(AlignedLatencyNode),
                                 &alignedNode, sizeof(AlignedLatencyNode)));
        }
    } else {
        // For host-device latency, initialize pattern with host addresses
        AlignedLatencyNode* hostMem = (AlignedLatencyNode*)dataBuffer.getBuffer();
        for (uint64_t i = 0; i < n_ptrs; i++) {
            hostMem[i].next = &hostMem[(i + strideLen) % n_ptrs];
        }
    }
}

template void Testcase::latencyHelper<struct LatencyNode>(const MemcpyBuffer &dataBuffer, bool measureDeviceToDeviceLatency);
template void Testcase::latencyHelper<struct TMALatencyNode>(const MemcpyBuffer &dataBuffer, bool measureDeviceToDeviceLatency);

void Testcase::forEachAllToOneBufferPairing(unsigned long long size, MemcpyOperation &memcpyInstance, bool isAllToOne, allToOneOneToAllCallback callback) {
    const bool isNonVmBuffer = memcpyInstance.getP2PType() == P2PType::SM || gSettings.disableVm;
    const bool useSubReadCmd =
        !gSettings.disableVm &&
        (detectIcnRemoteType(deviceCount - 1) == sailbandwidthIcnRemoteType::Switch) &&
        (memcpyInstance.getP2PType() == P2PType::CE) &&
        (memcpyInstance.ctxPreference == PREFER_DST_CONTEXT) &&
        (memcpyInstance.icnReadConfig.enableSplitSizeReadOptimize ||
         memcpyInstance.icnReadConfig.enableVpipeIdReadOptimize);

    std::vector<unsigned long long> splitSizes;
    if (useSubReadCmd) {
        int maxSplitCount = PPU_ICN_LINK_PER_DIE();
        int splitCount = (size < maxSplitCount) ? static_cast<int>(size) : maxSplitCount;
        if (splitCount == 0) {
            return;
        }
        // Split the transfer across link-aligned chunks to improve parallel link utilization
        unsigned long long baseSplitSize = size / splitCount;
        unsigned long long remainder = size % splitCount;

        for (int i = 0; i < splitCount; i++) {
            unsigned long long currentSplitSize = baseSplitSize;
            if (i == splitCount - 1) {
                currentSplitSize += remainder;
            }
            splitSizes.push_back(currentSplitSize);
        }
    } else {
        splitSizes.push_back(size);
    }

    std::vector<std::vector<MemcpyBuffer*>> allBuffersPreallocated(deviceCount);

    // allocate the all-group buffers up front, re-use to avoid reallocation
    for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
        for (auto splitSize : splitSizes) {
            allBuffersPreallocated[deviceId].push_back(isNonVmBuffer ?
                static_cast<MemcpyBuffer*>(new DeviceBuffer(splitSize, deviceId)) :
                static_cast<MemcpyBuffer*>(new DeviceBufferVm(splitSize, deviceId)));
        }
    }

    for (int oneDeviceId = 0; oneDeviceId < deviceCount; oneDeviceId++) {
        std::vector<const MemcpyBuffer*> oneBuffers;
        std::vector<const MemcpyBuffer*> allBuffers;

        for (int allDeviceId = 0; allDeviceId < deviceCount; allDeviceId++) {
            if (allDeviceId == oneDeviceId) {
                continue;
            }

            for (int i = 0; i < splitSizes.size(); i++) {
                MemcpyBuffer* oneBuffer = isNonVmBuffer ?
                    static_cast<MemcpyBuffer*>(new DeviceBuffer(splitSizes[i], oneDeviceId)) :
                    static_cast<MemcpyBuffer*>(new DeviceBufferVm(splitSizes[i], oneDeviceId));
                MemcpyBuffer* allBuffer = allBuffersPreallocated[allDeviceId][i];
                MemcpyBuffer* srcBuffer = isAllToOne ? allBuffer : oneBuffer;
                MemcpyBuffer* dstBuffer = isAllToOne ? oneBuffer : allBuffer;

                bool peerAccessEnabled = false;
                if (isNonVmBuffer) {
                    peerAccessEnabled = static_cast<DeviceBuffer*>(srcBuffer)->enablePeerAccess(
                        *static_cast<DeviceBuffer*>(dstBuffer));
                } else {
                    // Use virtual memory to prevent read operations from being converted into write operations.
                    peerAccessEnabled = static_cast<DeviceBufferVm*>(srcBuffer)->enableReadOnlyAccessFromPeer(
                        *static_cast<DeviceBufferVm*>(dstBuffer));
                }

                if (!peerAccessEnabled) {
                    delete oneBuffer;
                    continue;
                }

                allBuffers.push_back(allBuffer);
                oneBuffers.push_back(oneBuffer);
            }
        }
        // If no peer PPUs, skip measurements.
        if (!allBuffers.empty()) {
            callback(oneDeviceId, allBuffers, oneBuffers);
        }

        for (auto node : oneBuffers) {
            delete node;
        }
    }

    for (auto& buffers : allBuffersPreallocated) {
        for (auto node : buffers) {
            delete node;
        }
    }
}

void Testcase::allToOneHelper(unsigned long long size, MemcpyOperation &memcpyInstance, PeerValueMatrix<double> &bandwidthValues) {
    forEachAllToOneBufferPairing(size, memcpyInstance, true, [&](int oneDeviceId,
                                            std::vector<const MemcpyBuffer*>& allBuffers,
                                            std::vector<const MemcpyBuffer*>& oneBuffers) {
        if (memcpyInstance.ctxPreference == PREFER_DST_CONTEXT) {
            // Do a read from each PPU in the all group, by reading from the one PPU
            bandwidthValues.value(0, oneDeviceId) = memcpyInstance.doMemcpy(oneBuffers, allBuffers);
        } else {
            // Do a write from each PPU in the all group, by writing to the one PPU
            bandwidthValues.value(0, oneDeviceId) = memcpyInstance.doMemcpy(allBuffers, oneBuffers);
        }
    });
    recordStability(bandwidthValues, memcpyInstance);
}

void Testcase::oneToAllHelper(unsigned long long size, MemcpyOperation &memcpyInstance, PeerValueMatrix<double> &bandwidthValues) {
    forEachAllToOneBufferPairing(size, memcpyInstance, false, [&](int oneDeviceId,
                                            std::vector<const MemcpyBuffer*>& allBuffers,
                                            std::vector<const MemcpyBuffer*>& oneBuffers) {
        if (memcpyInstance.ctxPreference == PREFER_DST_CONTEXT) {
            // Do a read from the PPU in the one group, by reading from each PPU in the all group
            bandwidthValues.value(0, oneDeviceId) = memcpyInstance.doMemcpy(allBuffers, oneBuffers);
        } else {
            // Do a write from the PPU in the one group, by writing to each PPU in the all group
            bandwidthValues.value(0, oneDeviceId) = memcpyInstance.doMemcpy(oneBuffers, allBuffers);
        }
    });
    recordStability(bandwidthValues, memcpyInstance);
}

void Testcase::allHostHelper(unsigned long long size, MemcpyOperation &memcpyInstance, PeerValueMatrix<double> &bandwidthValues, bool sourceIsHost) {
    for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
        std::vector<const MemcpyBuffer*> deviceBuffers;
        std::vector<const MemcpyBuffer*> hostBuffers;

        deviceBuffers.push_back(new DeviceBuffer(size, deviceId));
        hostBuffers.push_back(new HostBuffer(size, deviceId));

        for (int interferenceDeviceId = 0; interferenceDeviceId < deviceCount; interferenceDeviceId++) {
            if (interferenceDeviceId == deviceId) {
                continue;
            }

            // Double the size of the interference copy to ensure it interferes correctly
            deviceBuffers.push_back(new DeviceBuffer(size * 2, interferenceDeviceId));
            hostBuffers.push_back(new HostBuffer(size * 2, interferenceDeviceId));
        }

        if (sourceIsHost) {
            bandwidthValues.value(0, deviceId) = memcpyInstance.doMemcpy(hostBuffers, deviceBuffers);
        } else {
            bandwidthValues.value(0, deviceId) = memcpyInstance.doMemcpy(deviceBuffers, hostBuffers);
        }

        for (auto node : deviceBuffers) {
            delete node;
        }

        for (auto node : hostBuffers) {
            delete node;
        }
    }
    recordStability(bandwidthValues, memcpyInstance);
}

void Testcase::allHostBidirHelper(unsigned long long size, MemcpyOperation &memcpyInstance, PeerValueMatrix<double> &bandwidthValues, bool sourceIsHost) {
    for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
        std::vector<const MemcpyBuffer*> srcBuffers;
        std::vector<const MemcpyBuffer*> dstBuffers;

        if (sourceIsHost) {
            srcBuffers.push_back(new HostBuffer(size, deviceId));
            dstBuffers.push_back(new DeviceBuffer(size, deviceId));

            // Double the size of the interference copy to ensure it interferes correctly
            srcBuffers.push_back(new DeviceBuffer(size * 2, deviceId));
            dstBuffers.push_back(new HostBuffer(size * 2, deviceId));
        } else {
            srcBuffers.push_back(new DeviceBuffer(size, deviceId));
            dstBuffers.push_back(new HostBuffer(size, deviceId));

            // Double the size of the interference copy to ensure it interferes correctly
            srcBuffers.push_back(new HostBuffer(size * 2, deviceId));
            dstBuffers.push_back(new DeviceBuffer(size * 2, deviceId));
        }

        for (int interferenceDeviceId = 0; interferenceDeviceId < deviceCount; interferenceDeviceId++) {
            if (interferenceDeviceId == deviceId) {
                continue;
            }

            // Double the size of the interference copy to ensure it interferes correctly
            srcBuffers.push_back(new DeviceBuffer(size * 2, interferenceDeviceId));
            dstBuffers.push_back(new HostBuffer(size * 2, interferenceDeviceId));

            srcBuffers.push_back(new HostBuffer(size * 2, interferenceDeviceId));
            dstBuffers.push_back(new DeviceBuffer(size * 2, interferenceDeviceId));
        }

        bandwidthValues.value(0, deviceId) = memcpyInstance.doMemcpy(srcBuffers, dstBuffers);

        for (auto node : srcBuffers) {
            delete node;
        }

        for (auto node : dstBuffers) {
            delete node;
        }
    }
    recordStability(bandwidthValues, memcpyInstance);
}

