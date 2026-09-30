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
#include "memcpy.h"

void HostToDeviceCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE());

    for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
        HostBuffer hostBuffer(size, deviceId);
        DeviceBuffer deviceBuffer(size, deviceId);

        bandwidthValues.value(0, deviceId) = memcpyInstance.doMemcpy(hostBuffer, deviceBuffer);
    }

    output->addTestcaseResults(bandwidthValues, "memcpy CE CPU(row) -> PPU(column) bandwidth (GB/s)");
}

void DeviceToHostCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE());

    for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
        HostBuffer hostBuffer(size, deviceId);
        DeviceBuffer deviceBuffer(size, deviceId);

        bandwidthValues.value(0, deviceId) = memcpyInstance.doMemcpy(deviceBuffer, hostBuffer);
    }

    output->addTestcaseResults(bandwidthValues, "memcpy CE CPU(row) <- PPU(column) bandwidth (GB/s)");
}

void HostToDeviceBidirCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE());

    for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
        // Double the size of the interference copy to ensure it interferes correctly
        HostBuffer host1(size, deviceId), host2(size * 2, deviceId);
        DeviceBuffer dev1(size, deviceId), dev2(size * 2, deviceId);

        std::vector<const MemcpyBuffer*> srcBuffers = {&host1, &dev2};
        std::vector<const MemcpyBuffer*> dstBuffers = {&dev1, &host2};

        bandwidthValues.value(0, deviceId) = memcpyInstance.doMemcpy(srcBuffers, dstBuffers);
    }

    output->addTestcaseResults(bandwidthValues, "memcpy CE CPU(row) <-> PPU(column) bandwidth (GB/s)");
}

void DeviceToHostBidirCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE());

    for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
        // Double the size of the interference copy to ensure it interferes correctly
        HostBuffer host1(size, deviceId), host2(size * 2, deviceId);
        DeviceBuffer dev1(size, deviceId), dev2(size * 2, deviceId);

        std::vector<const MemcpyBuffer*> srcBuffers = {&dev1, &host2};
        std::vector<const MemcpyBuffer*> dstBuffers = {&host1, &dev2};

        bandwidthValues.value(0, deviceId) = memcpyInstance.doMemcpy(srcBuffers, dstBuffers);
    }

    output->addTestcaseResults(bandwidthValues, "memcpy CE CPU(row) <-> PPU(column) bandwidth (GB/s)");
}

// DtoD Read test - copy from dst to src (backwards) using src context
void DeviceToDeviceReadCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(deviceCount, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), PREFER_DST_CONTEXT);
    
    if(ppuVpipeIdEnable) {
        memcpyInstance.icnReadConfig.useSingleLinkForIcnRead = useSingleLinkForIcnRead;
    }

    for (int srcDeviceId = 0; srcDeviceId < deviceCount; srcDeviceId++) {
        for (int peerDeviceId = 0; peerDeviceId < deviceCount; peerDeviceId++) {
            if (!testLocalCopy && peerDeviceId == srcDeviceId) {
                continue;
            }

            if (disableVm) {
                DeviceBuffer srcBuffer(size, srcDeviceId);
                DeviceBuffer peerBuffer(size, peerDeviceId);

                if (peerDeviceId != srcDeviceId) {
                    if (!srcBuffer.enablePeerAccess(peerBuffer)) {
                        continue;
                    }
                }
                // swap src and peer nodes, but use srcBuffers (the copy's destination) context
                bandwidthValues.value(srcDeviceId, peerDeviceId) = memcpyInstance.doMemcpy(peerBuffer, srcBuffer);
            } else {
                DeviceBufferVm srcBuffer(size, srcDeviceId);
                DeviceBufferVm peerBuffer(size, peerDeviceId);

                if (peerDeviceId != srcDeviceId) {
                    if (!srcBuffer.enableReadOnlyAccessFromPeer(peerBuffer)) {
                        continue;
                    }
                }
                bandwidthValues.value(srcDeviceId, peerDeviceId) = memcpyInstance.doMemcpy(peerBuffer, srcBuffer);
            }
        }
    }

    output->addTestcaseResults(bandwidthValues, "memcpy CE PPU(row) -> PPU(column) bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValues, false, "memcpy CE PPU(row) -> PPU(column)");
}

// DtoD Write test - copy from src to dst using src context
void DeviceToDeviceWriteCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(deviceCount, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE());

    for (int srcDeviceId = 0; srcDeviceId < deviceCount; srcDeviceId++) {
        for (int peerDeviceId = 0; peerDeviceId < deviceCount; peerDeviceId++) {
            if (!testLocalCopy && peerDeviceId == srcDeviceId) {
                continue;
            }

            DeviceBuffer srcBuffer(size, srcDeviceId);
            DeviceBuffer peerBuffer(size, peerDeviceId);

            if (peerDeviceId != srcDeviceId) {
                if (!srcBuffer.enablePeerAccess(peerBuffer)) {
                    continue;
                }
            }

            bandwidthValues.value(srcDeviceId, peerDeviceId) = memcpyInstance.doMemcpy(srcBuffer, peerBuffer);
        }
    }

    output->addTestcaseResults(bandwidthValues, "memcpy CE PPU(row) <- PPU(column) bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValues, false, "memcpy CE PPU(row) <- PPU(column)");
}

// DtoD Bidir Read test - copy from dst to src (backwards) using src context
void DeviceToDeviceBidirReadCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValuesRead1(deviceCount, deviceCount, key + "_read1");
    PeerValueMatrix<double> bandwidthValuesRead2(deviceCount, deviceCount, key + "_read2");
    PeerValueMatrix<double> bandwidthValuesTotal(deviceCount, deviceCount, key + "_total");
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), PREFER_DST_CONTEXT, MemcpyOperation::VECTOR_BW);
    std::vector<double> results;

    for (int srcDeviceId = 0; srcDeviceId < deviceCount; srcDeviceId++) {
        for (int peerDeviceId = 0; peerDeviceId < deviceCount; peerDeviceId++) {
            if (!testLocalCopy && peerDeviceId == srcDeviceId) {
                continue;
            }

            if (disableVm) {
                DeviceBuffer src1(size, srcDeviceId), src2(size, srcDeviceId);
                DeviceBuffer peer1(size, peerDeviceId), peer2(size, peerDeviceId);

                if (peerDeviceId != srcDeviceId) {
                    if (!src1.enablePeerAccess(peer1)) {
                        continue;
                    }
                }
                // swap src and peer nodes, but use srcBuffers (the copy's destination) context
                std::vector<const MemcpyBuffer*> srcBuffers = {&peer1, &src2};
                std::vector<const MemcpyBuffer*> peerBuffers = {&src1, &peer2};
                results = memcpyInstance.doMemcpyVector(srcBuffers, peerBuffers);
            } else {
                DeviceBufferVm src1(size, srcDeviceId), src2(size, srcDeviceId);
                DeviceBufferVm peer1(size, peerDeviceId), peer2(size, peerDeviceId);

                if (peerDeviceId != srcDeviceId) {
                    if (!src1.enableReadOnlyAccessFromPeer(peer1)) {
                        continue;
                    }
                    if (!peer2.enableReadOnlyAccessFromPeer(src2)) {
                        continue;
                    }
                }
                std::vector<const MemcpyBuffer*> srcBuffers = {&peer1, &src2};
                std::vector<const MemcpyBuffer*> peerBuffers = {&src1, &peer2};
                results = memcpyInstance.doMemcpyVector(srcBuffers, peerBuffers);
            }

            bandwidthValuesRead1.value(srcDeviceId, peerDeviceId) = results[0];
            bandwidthValuesRead2.value(srcDeviceId, peerDeviceId) = results[1];
            bandwidthValuesTotal.value(srcDeviceId, peerDeviceId) = results[0] + results[1];
        }
    }

    output->addTestcaseResults(bandwidthValuesRead1, "memcpy CE PPU(row) <-> PPU(column) Read1 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesRead2, "memcpy CE PPU(row) <-> PPU(column) Read2 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesTotal, "memcpy CE PPU(row) <-> PPU(column) Total bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValuesTotal, false, "memcpy CE PPU(row) <-> PPU(column) Total");
}

// DtoD Bidir Write test - copy from src to dst using src context
void DeviceToDeviceBidirWriteCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValuesWrite1(deviceCount, deviceCount, key + "_write1");
    PeerValueMatrix<double> bandwidthValuesWrite2(deviceCount, deviceCount, key + "_write2");
    PeerValueMatrix<double> bandwidthValuesTotal(deviceCount, deviceCount, key + "_total");
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), PREFER_SRC_CONTEXT, MemcpyOperation::VECTOR_BW);

    for (int srcDeviceId = 0; srcDeviceId < deviceCount; srcDeviceId++) {
        for (int peerDeviceId = 0; peerDeviceId < deviceCount; peerDeviceId++) {
            if (!testLocalCopy && peerDeviceId == srcDeviceId) {
                continue;
            }

            DeviceBuffer src1(size, srcDeviceId), src2(size, srcDeviceId);
            DeviceBuffer peer1(size, peerDeviceId), peer2(size, peerDeviceId);

            if (peerDeviceId != srcDeviceId) {
                if (!src1.enablePeerAccess(peer1)) {
                    continue;
                }
            }

            // swap src and peer nodes, but use srcBuffers (the copy's destination) context
            std::vector<const MemcpyBuffer*> srcBuffers = {&peer1, &src2};
            std::vector<const MemcpyBuffer*> peerBuffers = {&src1, &peer2};

            auto results = memcpyInstance.doMemcpyVector(srcBuffers, peerBuffers);
            bandwidthValuesWrite1.value(srcDeviceId, peerDeviceId) = results[0];
            bandwidthValuesWrite2.value(srcDeviceId, peerDeviceId) = results[1];
            bandwidthValuesTotal.value(srcDeviceId, peerDeviceId) = results[0] + results[1];
        }
    }

    output->addTestcaseResults(bandwidthValuesWrite1, "memcpy CE PPU(row) <-> PPU(column) Write1 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesWrite2, "memcpy CE PPU(row) <-> PPU(column) Write2 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesTotal, "memcpy CE PPU(row) <-> PPU(column) Total bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValuesTotal, false, "memcpy CE PPU(row) <-> PPU(column) Total");
}

void DeviceLocalCopy::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE());

    for (int deviceId = 0; deviceId < deviceCount; deviceId++) {
        DeviceBuffer deviceBuffer1(size, deviceId);
        DeviceBuffer deviceBuffer2(size, deviceId);

        bandwidthValues.value(0, deviceId) = memcpyInstance.doMemcpy(deviceBuffer2, deviceBuffer1);
    }

    output->addTestcaseResults(bandwidthValues, "memcpy local PPU(column) bandwidth (GB/s)");
}

void AllToHostCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE());

    allHostHelper(size, memcpyInstance, bandwidthValues, false);

    output->addTestcaseResults(bandwidthValues, "memcpy CE CPU(row) <- PPU(column) bandwidth (GB/s)");
}

void AllToHostBidirCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE());

    allHostBidirHelper(size, memcpyInstance, bandwidthValues, false);

    output->addTestcaseResults(bandwidthValues, "memcpy CE CPU(row) <-> PPU(column) bandwidth (GB/s)");
}

void HostToAllCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE());

    allHostHelper(size, memcpyInstance, bandwidthValues, true);

    output->addTestcaseResults(bandwidthValues, "memcpy CE CPU(row) -> PPU(column) bandwidth (GB/s)");
}

void HostToAllBidirCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE());

    allHostBidirHelper(size, memcpyInstance, bandwidthValues, true);

    output->addTestcaseResults(bandwidthValues, "memcpy CE CPU(row) <-> PPU(column) bandwidth (GB/s)");
}

// Write test - copy from src to dst using src context
void AllToOneWriteCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), PREFER_SRC_CONTEXT, MemcpyOperation::TOTAL_BW);
    allToOneHelper(size, memcpyInstance, bandwidthValues, false);

    output->addTestcaseResults(bandwidthValues, "memcpy CE PPU(row) <- PPU(column) bandwidth (GB/s)");
}

// Read test - copy from dst to src (backwards) using src context
void AllToOneReadCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), PREFER_DST_CONTEXT, MemcpyOperation::TOTAL_BW);
    memcpyInstance.icnReadConfig.enableSplitSizeReadOptimize = enableSplitSizeReadOptimize;
    if(ppuVpipeIdEnable) {
        memcpyInstance.icnReadConfig.useSingleLinkForIcnRead = useSingleLinkForIcnRead;
        memcpyInstance.icnReadConfig.enableVpipeIdReadOptimize = enableVpipeIdReadOptimize;
    }
    allToOneHelper(size, memcpyInstance, bandwidthValues, true);

    output->addTestcaseResults(bandwidthValues, "memcpy CE PPU(row) -> PPU(column) bandwidth (GB/s)");
}

// Write test - copy from src to dst using src context
void OneToAllWriteCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), PREFER_SRC_CONTEXT, MemcpyOperation::TOTAL_BW);
    oneToAllHelper(size, memcpyInstance, bandwidthValues, false);

    output->addTestcaseResults(bandwidthValues, "memcpy CE PPU(row) -> PPU(column) bandwidth (GB/s)");
}

// Read test - copy from dst to src (backwards) using src context
void OneToAllReadCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, deviceCount, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), PREFER_DST_CONTEXT, MemcpyOperation::TOTAL_BW);
    memcpyInstance.icnReadConfig.enableSplitSizeReadOptimize = enableSplitSizeReadOptimize;
    if(ppuVpipeIdEnable) {
        memcpyInstance.icnReadConfig.useSingleLinkForIcnRead = useSingleLinkForIcnRead;
        memcpyInstance.icnReadConfig.enableVpipeIdReadOptimize = enableVpipeIdReadOptimize;
    }
    oneToAllHelper(size, memcpyInstance, bandwidthValues, true);

    output->addTestcaseResults(bandwidthValues, "memcpy CE PPU(row) <- PPU(column) bandwidth (GB/s)");
}
