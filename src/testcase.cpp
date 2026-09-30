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

Testcase::Testcase(std::string key, std::string desc) :
    key(std::move(key)), desc(std::move(desc))
{}

std::string Testcase::testKey() { return key; }
std::string Testcase::testDesc() { return desc; }

bool Testcase::filterHasAccessiblePeerPairs() {
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
                return true;
            }
        }
    }

    return false;
}

bool Testcase::filterSupportsMulticast() {
    int deviceCount = 0;
    HG_ASSERT(hgDeviceGetCount(&deviceCount));

    for (int currentDevice = 0; currentDevice < deviceCount; currentDevice++) {
        HGdevice dev;
        HG_ASSERT(hgDeviceGet(&dev, currentDevice));
        int supportsMulticast = 0;

        HG_ASSERT(hgDeviceGetAttribute(&supportsMulticast, HG_DEVICE_ATTRIBUTE_MULTICAST_SUPPORTED, dev));
        if (!supportsMulticast) {
            return false;
        }
    }

    return true;
}

#ifdef MULTINODE
// Each MPI rank handles one PPU, so we simply have to check if we have more than 1 process
bool Testcase::filterHasMultiplePPUsMultinode() {
    return worldSize > 1;
}
#endif

void Testcase::latencyHelper(const MemcpyBuffer &dataBuffer, bool measureDeviceToDeviceLatency) {
    uint64_t n_ptrs = dataBuffer.getBufferSize() / sizeof(struct LatencyNode);

    if (measureDeviceToDeviceLatency) {
        // For device-to-device latency, create and initialize pattern on device
        for (uint64_t i = 0; i < n_ptrs; i++) {
            struct LatencyNode node;
            size_t nextOffset = ((i + strideLen) % n_ptrs) * sizeof(struct LatencyNode);
            // Set up pattern with device addresses
            node.next = (struct LatencyNode*)(dataBuffer.getBuffer() + nextOffset);
            HG_ASSERT(hgMemcpyHtoD(dataBuffer.getBuffer() + i*sizeof(struct LatencyNode),
                                 &node, sizeof(struct LatencyNode)));
        }
    } else {
        // For host-device latency, initialize pattern with host addresses
        struct LatencyNode* hostMem = (struct LatencyNode*)dataBuffer.getBuffer();
        for (uint64_t i = 0; i < n_ptrs; i++) {
            hostMem[i].next = &hostMem[(i + strideLen) % n_ptrs];
        }
    }
}

void Testcase::allToOneHelper(unsigned long long size, MemcpyOperation &memcpyInstance, PeerValueMatrix<double> &bandwidthValues, bool isRead) {
    const bool useSubReadCmd =
        (detectIcnRemoteType(deviceCount - 1) == sailbandwidthIcnRemoteType::Switch) &&
        (memcpyInstance.getP2PType() == P2PType::CE) &&
        isRead &&
        (memcpyInstance.icnReadConfig.enableSplitSizeReadOptimize ||
         memcpyInstance.icnReadConfig.enableVpipeIdReadOptimize);
    if (useSubReadCmd) {
        int maxSplitCount = PPU_ICN_LINK_PER_DIE();
        int splitCount = (size < maxSplitCount) ? static_cast<int>(size) : maxSplitCount;
        if (splitCount == 0) {
            return;
        }
        // Split the transfer across link-aligned chunks to improve parallel link utilization
        unsigned long long baseSplitSize = size / splitCount;
        unsigned long long remainder = size % splitCount;

        std::vector<unsigned long long> splitSizes;
        for (int i = 0; i < splitCount; i++) {
            unsigned long long currentSplitSize = baseSplitSize;
            if (i == splitCount - 1) {
                currentSplitSize += remainder;
            }
            splitSizes.push_back(currentSplitSize);
        }

        std::vector<std::vector<DeviceBufferVm*>> allSrcBuffers(deviceCount);
        for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
            for (int i = 0; i < splitCount; i++) {
                allSrcBuffers[deviceId].push_back(new DeviceBufferVm(splitSizes[i], deviceId));
            }
        }

        // Allocate all src nodes up front, re-use to avoid reallocation.
        for (int dstDeviceId = 0; dstDeviceId < deviceCount; dstDeviceId++) {
            std::vector<const MemcpyBuffer*> flatSrcBuffers;
            std::vector<const MemcpyBuffer*> flatDstBuffers;

            for (int srcDeviceId = 0; srcDeviceId < deviceCount; srcDeviceId++) {
                if (srcDeviceId == dstDeviceId) {
                    continue;
                }

                for (int i = 0; i < splitCount; i++) {
                    // Use virtual memory to prevent read operations from being converted into write operations.
                    DeviceBufferVm* dstBuffer = new DeviceBufferVm(splitSizes[i], dstDeviceId);

                    if (!allSrcBuffers[srcDeviceId][i]->enableReadOnlyAccessFromPeer(*dstBuffer)) {
                        delete dstBuffer;
                        continue;
                    }

                    flatSrcBuffers.push_back(allSrcBuffers[srcDeviceId][i]);
                    flatDstBuffers.push_back(dstBuffer);
                }
            }

            // If no peer PPUs, skip measurements.
            if (!flatSrcBuffers.empty()) {
                // Swap dst and src for read tests so the copy is launched from the destination context.
                bandwidthValues.value(0, dstDeviceId) = memcpyInstance.doMemcpy(flatDstBuffers, flatSrcBuffers);
            }

            for (auto node : flatDstBuffers) {
                delete node;
            }
        }

        for (auto& vec : allSrcBuffers) {
            for (auto node : vec) {
                delete node;
            }
        }
    } else {
        const bool isNonVmBuffer = memcpyInstance.getP2PType() == P2PType::SM || disableVm;
        std::vector<MemcpyBuffer*> allSrcBuffers;

        for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
            if (isNonVmBuffer) {
                allSrcBuffers.push_back(new DeviceBuffer(size, deviceId));
            } else {
                allSrcBuffers.push_back(new DeviceBufferVm(size, deviceId));
            }
        }

        for (int dstDeviceId = 0; dstDeviceId < deviceCount; dstDeviceId++) {
            std::vector<const MemcpyBuffer*> dstBuffers;
            std::vector<const MemcpyBuffer*> srcBuffers;

            for (int srcDeviceId = 0; srcDeviceId < deviceCount; srcDeviceId++) {
                if (srcDeviceId == dstDeviceId) {
                    continue;
                }

                MemcpyBuffer* dstBuffer = isNonVmBuffer ? 
                                            static_cast<MemcpyBuffer*>(new DeviceBuffer(size, dstDeviceId)) : 
                                            static_cast<MemcpyBuffer*>(new DeviceBufferVm(size, dstDeviceId));

                bool peerAccessEnabled = false;
                if (isNonVmBuffer) {
                    auto* srcDeviceBuffer = static_cast<DeviceBuffer*>(allSrcBuffers[srcDeviceId]);
                    auto* dstDeviceBuffer = static_cast<DeviceBuffer*>(dstBuffer);
                    peerAccessEnabled = srcDeviceBuffer->enablePeerAccess(*dstDeviceBuffer);
                } else {
                    auto* srcDeviceBuffer = static_cast<DeviceBufferVm*>(allSrcBuffers[srcDeviceId]);
                    auto* dstDeviceBuffer = static_cast<DeviceBufferVm*>(dstBuffer);
                    peerAccessEnabled = srcDeviceBuffer->enableReadOnlyAccessFromPeer(*dstDeviceBuffer);
                }

                if (!peerAccessEnabled) {
                    delete dstBuffer;
                    continue;
                }

                srcBuffers.push_back(allSrcBuffers[srcDeviceId]);
                dstBuffers.push_back(dstBuffer);
            }

            if (!srcBuffers.empty()) {
                if(isRead) {
                    bandwidthValues.value(0, dstDeviceId) = memcpyInstance.doMemcpy(dstBuffers, srcBuffers);
                } else {
                    bandwidthValues.value(0, dstDeviceId) = memcpyInstance.doMemcpy(srcBuffers, dstBuffers);
                }
            }

            for (auto node : dstBuffers) {
                delete node;
            }
        }

        for (auto node : allSrcBuffers) {
            delete node;
        }
    }
}

void Testcase::oneToAllHelper(unsigned long long size, MemcpyOperation &memcpyInstance, PeerValueMatrix<double> &bandwidthValues, bool isRead) {
    const bool useSubReadCmd =
        (detectIcnRemoteType(deviceCount - 1) == sailbandwidthIcnRemoteType::Switch) &&
        (memcpyInstance.getP2PType() == P2PType::CE) &&
        isRead &&
        (memcpyInstance.icnReadConfig.enableSplitSizeReadOptimize ||
         memcpyInstance.icnReadConfig.enableVpipeIdReadOptimize);
    if (useSubReadCmd) {
        int maxSplitCount = PPU_ICN_LINK_PER_DIE();
        int splitCount = (size < maxSplitCount) ? static_cast<int>(size) : maxSplitCount;

        if (splitCount == 0) {
            return;
        }

        unsigned long long baseSplitSize = size / splitCount;
        unsigned long long remainder = size % splitCount;

        std::vector<unsigned long long> splitSizes;
        for (int i = 0; i < splitCount; i++) {
            unsigned long long currentSplitSize = baseSplitSize;
            if (i == splitCount - 1) {
                currentSplitSize += remainder;
            }
            splitSizes.push_back(currentSplitSize);
        }

        std::vector<std::vector<DeviceBufferVm*>> allDstBuffers(deviceCount);

        // allocate all src nodes up front, re-use to avoid reallocation
        for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
            for (int i = 0; i < splitCount; i++) {
                allDstBuffers[deviceId].push_back(new DeviceBufferVm(splitSizes[i], deviceId));
            }
        }
        
        for (int srcDeviceId = 0; srcDeviceId < deviceCount; srcDeviceId++) {
            std::vector<const MemcpyBuffer*> flatSrcBuffers;
            std::vector<const MemcpyBuffer*> flatDstBuffers;

            for (int dstDeviceId = 0; dstDeviceId < deviceCount; dstDeviceId++) {
                if (srcDeviceId == dstDeviceId) {
                    continue;
                }

                for (int i = 0; i < splitCount; i++) {
                    DeviceBufferVm* srcBuffer = new DeviceBufferVm(splitSizes[i], srcDeviceId);
                    if (!srcBuffer->enableReadOnlyAccessFromPeer(*allDstBuffers[dstDeviceId][i])) {
                        delete srcBuffer;
                        continue;
                    }

                    flatSrcBuffers.push_back(srcBuffer);
                    flatDstBuffers.push_back(allDstBuffers[dstDeviceId][i]);
                }
            }
            // If no peer PPUs, skip measurements.
            if (!flatSrcBuffers.empty()) {
                // swap dst and src for read tests
                bandwidthValues.value(0, srcDeviceId) = memcpyInstance.doMemcpy(flatDstBuffers, flatSrcBuffers);
            }

            for (auto node : flatSrcBuffers) {
                delete node;
            }
        }

        for (auto& vec : allDstBuffers) {
            for (auto node : vec) {
                delete node;
            }
        }
    } else {
        const bool isNonVmBuffer = memcpyInstance.getP2PType() == P2PType::SM || disableVm;
        std::vector<MemcpyBuffer*> allDstBuffers;

        // allocate all dst nodes up front, re-use to avoid reallocation
        for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
            if (isNonVmBuffer) {
                allDstBuffers.push_back(new DeviceBuffer(size, deviceId));
            } else {
                allDstBuffers.push_back(new DeviceBufferVm(size, deviceId));
            }
        }

        for (int srcDeviceId = 0; srcDeviceId < deviceCount; srcDeviceId++) {
            std::vector<const MemcpyBuffer*> dstBuffers;
            std::vector<const MemcpyBuffer*> srcBuffers;

            for (int dstDeviceId = 0; dstDeviceId < deviceCount; dstDeviceId++) {
                if (srcDeviceId == dstDeviceId) {
                    continue;
                }

                MemcpyBuffer* srcBuffer = isNonVmBuffer ? 
                                            static_cast<MemcpyBuffer*>(new DeviceBuffer(size, srcDeviceId)) : 
                                            static_cast<MemcpyBuffer*>(new DeviceBufferVm(size, srcDeviceId));

                bool peerAccessEnabled = false;
                if (isNonVmBuffer) {
                    auto* srcDeviceBuffer = static_cast<DeviceBuffer*>(srcBuffer);
                    auto* dstDeviceBuffer = static_cast<DeviceBuffer*>(allDstBuffers[dstDeviceId]);
                    peerAccessEnabled = srcDeviceBuffer->enablePeerAccess(*dstDeviceBuffer);
                } else {
                    auto* srcDeviceBuffer = static_cast<DeviceBufferVm*>(srcBuffer);
                    auto* dstDeviceBuffer = static_cast<DeviceBufferVm*>(allDstBuffers[dstDeviceId]);
                    peerAccessEnabled = srcDeviceBuffer->enableReadOnlyAccessFromPeer(*dstDeviceBuffer);
                }

                if (!peerAccessEnabled) {
                    delete srcBuffer;
                    continue;
                }

                srcBuffers.push_back(srcBuffer);
                dstBuffers.push_back(allDstBuffers[dstDeviceId]);
            }
            // If no peer PPUs, skip measurements.
            if ( !srcBuffers.empty() ) {
                if (isRead) {
                    // swap dst and src for read tests
                    bandwidthValues.value(0, srcDeviceId) = memcpyInstance.doMemcpy(dstBuffers, srcBuffers);
                } else {
                    bandwidthValues.value(0, srcDeviceId) = memcpyInstance.doMemcpy(srcBuffers, dstBuffers);
                }
            }

            for (auto node : srcBuffers) {
                delete node;
            }
        }

        for (auto node : allDstBuffers) {
            delete node;
        }
    }
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
}

