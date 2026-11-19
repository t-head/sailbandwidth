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

#include "testcase.h"
#include "memcpy.h"
#include "ppu_common.h"
#include "output.h"
#ifdef MULTINODE
#include <mpi.h>
#include <memory>
#include <set>
#include "multinode_memcpy.h"

void Testcase::multinodeConcurrentPairHelper(unsigned long long size, unsigned long long loopCount, MemcpyInitiator *memcpyInitiator,
                                              bool isRead, PeerValueMatrix<double> &bandwidthValues) {
    auto *phaseHelper = new NodeHelperMulti(true);
    MemcpyOperation memcpyInstance(loopCount, memcpyInitiator, phaseHelper, isRead ? PREFER_DST_CONTEXT : PREFER_SRC_CONTEXT,
                                   MemcpyOperation::VECTOR_BW);
    const bool copyFromPeer = isRead;

    const auto phases = planConcurrentPairPhases(generateTestPairs(worldSize, gSettings.targetNumPairs), false);
    for (size_t phaseIndex = 0; phaseIndex < phases.size(); phaseIndex++) {
        phaseHelper->beginPhase(phaseIndex + 1, phases.size());
        const auto &phase = phases[phaseIndex];
        std::vector<std::unique_ptr<MultinodeDeviceBufferUnicast>> buffers;
        std::vector<const MemcpyBuffer*> srcNodes;
        std::vector<const MemcpyBuffer*> dstNodes;
        for (const auto& pair : phase) {
            buffers.emplace_back(new MultinodeDeviceBufferUnicast(size, pair.first));
            const MemcpyBuffer *srcNode = buffers.back().get();
            buffers.emplace_back(new MultinodeDeviceBufferUnicast(size, pair.second));
            const MemcpyBuffer *peerNode = buffers.back().get();

            srcNodes.push_back(copyFromPeer ? peerNode : srcNode);
            dstNodes.push_back(copyFromPeer ? srcNode : peerNode);
        }

        auto results = memcpyInstance.doMemcpyVector(srcNodes, dstNodes);
        for (size_t i = 0; i < phase.size(); i++) {
            bandwidthValues.value(phase[i].first, phase[i].second) = results[i];
        }
    }

    recordStability(bandwidthValues, memcpyInstance);
}

void Testcase::multinodeConcurrentBidirPairHelper(unsigned long long size, unsigned long long loopCount, MemcpyInitiator *memcpyInitiator,
                                                   bool isRead, PeerValueMatrix<double> &bandwidthValues1,
                                                   PeerValueMatrix<double> &bandwidthValues2, PeerValueMatrix<double> &bandwidthValuesTotal) {
    auto *phaseHelper = new NodeHelperMulti(true);
    MemcpyOperation memcpyInstance(loopCount, memcpyInitiator, phaseHelper, isRead ? PREFER_DST_CONTEXT : PREFER_SRC_CONTEXT,
                                   MemcpyOperation::VECTOR_BW);
    const bool copyFromPeer = isRead;
    const auto selectedPairs = generateTestPairs(worldSize, gSettings.targetNumPairs);
    const std::set<std::pair<int, int>> selected(selectedPairs.begin(), selectedPairs.end());

    const auto phases = planConcurrentPairPhases(selectedPairs, true);
    for (size_t phaseIndex = 0; phaseIndex < phases.size(); phaseIndex++) {
        phaseHelper->beginPhase(phaseIndex + 1, phases.size());
        const auto &phase = phases[phaseIndex];
        std::vector<std::unique_ptr<MultinodeDeviceBufferUnicast>> buffers;
        std::vector<const MemcpyBuffer*> srcNodes;
        std::vector<const MemcpyBuffer*> dstNodes;
        for (const auto& pair : phase) {
            buffers.emplace_back(new MultinodeDeviceBufferUnicast(size, pair.first));
            const MemcpyBuffer *src1 = buffers.back().get();
            buffers.emplace_back(new MultinodeDeviceBufferUnicast(size, pair.first));
            const MemcpyBuffer *src2 = buffers.back().get();
            buffers.emplace_back(new MultinodeDeviceBufferUnicast(size, pair.second));
            const MemcpyBuffer *peer1 = buffers.back().get();
            buffers.emplace_back(new MultinodeDeviceBufferUnicast(size, pair.second));
            const MemcpyBuffer *peer2 = buffers.back().get();

            srcNodes.push_back(copyFromPeer ? peer1 : src1);
            dstNodes.push_back(copyFromPeer ? src1 : peer1);
            srcNodes.push_back(copyFromPeer ? src2 : peer2);
            dstNodes.push_back(copyFromPeer ? peer2 : src2);
        }

        auto results = memcpyInstance.doMemcpyVector(srcNodes, dstNodes);
        for (size_t i = 0; i < phase.size(); i++) {
            const int srcDeviceId = phase[i].first;
            const int peerDeviceId = phase[i].second;
            const double srcExecuted = results[2 * i];
            const double peerExecuted = results[2 * i + 1];
            if (selected.count({srcDeviceId, peerDeviceId}) != 0) {
                bandwidthValues1.value(srcDeviceId, peerDeviceId) = srcExecuted;
                bandwidthValues2.value(srcDeviceId, peerDeviceId) = peerExecuted;
                bandwidthValuesTotal.value(srcDeviceId, peerDeviceId) = srcExecuted + peerExecuted;
            }
            if (selected.count({peerDeviceId, srcDeviceId}) != 0) {
                bandwidthValues1.value(peerDeviceId, srcDeviceId) = peerExecuted;
                bandwidthValues2.value(peerDeviceId, srcDeviceId) = srcExecuted;
                bandwidthValuesTotal.value(peerDeviceId, srcDeviceId) = srcExecuted + peerExecuted;
            }
        }
    }

    recordStability(bandwidthValuesTotal, memcpyInstance);
}


// DtoD Read test - copy from dst to src (backwards) using src context
void MultinodeDeviceToDeviceReadCE::run(unsigned long long size, unsigned long long loopCount) {
    if (gSettings.isConcurrentPairSchedule()) {
        PeerValueMatrix<double> bandwidthValues(worldSize, worldSize, key);
        multinodeConcurrentPairHelper(size, loopCount, new MemcpyInitiatorCE(), true, bandwidthValues);
        output->addTestcaseResults(bandwidthValues, "memcpy CE PPU(row) -> PPU(column) bandwidth (GB/s)");
        output->displayUtilizationRatioMatrix(bandwidthValues, true, "memcpy CE PPU(row) -> PPU(column)");
        return;
    }

    PeerValueMatrix<double> bandwidthValues(worldSize, worldSize, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), new NodeHelperMulti(), PREFER_DST_CONTEXT);

    for (const auto& pair : generateTestPairs(worldSize, gSettings.targetNumPairs)) {
        int srcDeviceId = pair.first;
        int peerDeviceId = pair.second;

        MultinodeDeviceBufferUnicast srcNode(size, srcDeviceId);
        MultinodeDeviceBufferUnicast peerNode(size, peerDeviceId);

        // swap src and peer nodes, but use srcNodes (the copy's destination) context
        bandwidthValues.value(srcDeviceId, peerDeviceId) = memcpyInstance.doMemcpy(peerNode, srcNode);
    }

    recordStability(bandwidthValues, memcpyInstance);
    output->addTestcaseResults(bandwidthValues, "memcpy CE PPU(row) -> PPU(column) bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValues, true, "memcpy CE PPU(row) -> PPU(column)");
}


// DtoD Write test - copy from src to dst using src context
void MultinodeDeviceToDeviceWriteCE::run(unsigned long long size, unsigned long long loopCount) {
    if (gSettings.isConcurrentPairSchedule()) {
        PeerValueMatrix<double> bandwidthValues(worldSize, worldSize, key);
        multinodeConcurrentPairHelper(size, loopCount, new MemcpyInitiatorCE(), false, bandwidthValues);
        output->addTestcaseResults(bandwidthValues, "memcpy CE PPU(row) <- PPU(column) bandwidth (GB/s)");
        output->displayUtilizationRatioMatrix(bandwidthValues, true, "memcpy CE PPU(row) <- PPU(column)");
        return;
    }

    PeerValueMatrix<double> bandwidthValues(worldSize, worldSize, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), new NodeHelperMulti(), PREFER_SRC_CONTEXT);

    for (const auto& pair : generateTestPairs(worldSize, gSettings.targetNumPairs)) {
        int srcDeviceId = pair.first;
        int peerDeviceId = pair.second;

        MultinodeDeviceBufferUnicast srcNode(size, srcDeviceId);
        MultinodeDeviceBufferUnicast peerNode(size, peerDeviceId);

        bandwidthValues.value(srcDeviceId, peerDeviceId) = memcpyInstance.doMemcpy(srcNode, peerNode);
    }

    recordStability(bandwidthValues, memcpyInstance);
    output->addTestcaseResults(bandwidthValues, "memcpy CE PPU(row) <- PPU(column) bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValues, true, "memcpy CE PPU(row) <- PPU(column)");
}

// DtoD Bidir Read test - copy from dst to src (backwards) using src context
void MultinodeDeviceToDeviceBidirReadCE::run(unsigned long long size, unsigned long long loopCount) {
    if (gSettings.isConcurrentPairSchedule()) {
        PeerValueMatrix<double> bandwidthValuesRead1(worldSize, worldSize, key + "_read1");
        PeerValueMatrix<double> bandwidthValuesRead2(worldSize, worldSize, key + "_read2");
        PeerValueMatrix<double> bandwidthValuesTotal(worldSize, worldSize, key + "_total");
        multinodeConcurrentBidirPairHelper(size, loopCount, new MemcpyInitiatorCE(), true,
                                           bandwidthValuesRead1, bandwidthValuesRead2, bandwidthValuesTotal);
        output->addTestcaseResults(bandwidthValuesRead1, "memcpy CE PPU(row) <-> PPU(column) Read1 bandwidth (GB/s)");
        output->addTestcaseResults(bandwidthValuesRead2, "memcpy CE PPU(row) <-> PPU(column) Read2 bandwidth (GB/s)");
        output->addTestcaseResults(bandwidthValuesTotal, "memcpy CE PPU(row) <-> PPU(column) Total bandwidth (GB/s)");
        output->displayUtilizationRatioMatrix(bandwidthValuesTotal, true, "memcpy CE PPU(row) <-> PPU(column) Total");
        return;
    }

    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), new NodeHelperMulti(), PREFER_DST_CONTEXT, MemcpyOperation::VECTOR_BW);
    PeerValueMatrix<double> bandwidthValuesRead1(worldSize, worldSize, key + "_read1");
    PeerValueMatrix<double> bandwidthValuesRead2(worldSize, worldSize, key + "_read2");
    PeerValueMatrix<double> bandwidthValuesTotal(worldSize, worldSize, key + "_total");

    for (const auto& pair : generateTestPairs(worldSize, gSettings.targetNumPairs)) {
        int srcDeviceId = pair.first;
        int peerDeviceId = pair.second;

        MultinodeDeviceBufferUnicast src1(size, srcDeviceId), src2(size, srcDeviceId);
        MultinodeDeviceBufferUnicast peer1(size, peerDeviceId), peer2(size, peerDeviceId);

        // swap src and peer nodes, but use srcNodes (the copy's destination) context
        std::vector<const MemcpyBuffer*> srcNodes = {&peer1, &src2};
        std::vector<const MemcpyBuffer*> peerNodes = {&src1, &peer2};

        auto results = memcpyInstance.doMemcpyVector(srcNodes, peerNodes);
        bandwidthValuesRead1.value(srcDeviceId, peerDeviceId) = results[0];
        bandwidthValuesRead2.value(srcDeviceId, peerDeviceId) = results[1];
        bandwidthValuesTotal.value(srcDeviceId, peerDeviceId) = results[0] + results[1];
    }

    recordStability(bandwidthValuesTotal, memcpyInstance);
    output->addTestcaseResults(bandwidthValuesRead1, "memcpy CE PPU(row) <-> PPU(column) Read1 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesRead2, "memcpy CE PPU(row) <-> PPU(column) Read2 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesTotal, "memcpy CE PPU(row) <-> PPU(column) Total bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValuesTotal, true, "memcpy CE PPU(row) <-> PPU(column) Total");
}

// DtoD Bidir Write test - copy from src to dst using src context
void MultinodeDeviceToDeviceBidirWriteCE::run(unsigned long long size, unsigned long long loopCount) {
    if (gSettings.isConcurrentPairSchedule()) {
        PeerValueMatrix<double> bandwidthValuesWrite1(worldSize, worldSize, key + "_write1");
        PeerValueMatrix<double> bandwidthValuesWrite2(worldSize, worldSize, key + "_write2");
        PeerValueMatrix<double> bandwidthValuesTotal(worldSize, worldSize, key + "_total");
        multinodeConcurrentBidirPairHelper(size, loopCount, new MemcpyInitiatorCE(), false,
                                           bandwidthValuesWrite1, bandwidthValuesWrite2, bandwidthValuesTotal);
        output->addTestcaseResults(bandwidthValuesWrite1, "memcpy CE PPU(row) <-> PPU(column) Write1 bandwidth (GB/s)");
        output->addTestcaseResults(bandwidthValuesWrite2, "memcpy CE PPU(row) <-> PPU(column) Write2 bandwidth (GB/s)");
        output->addTestcaseResults(bandwidthValuesTotal, "memcpy CE PPU(row) <-> PPU(column) Total bandwidth (GB/s)");
        output->displayUtilizationRatioMatrix(bandwidthValuesTotal, true, "memcpy CE PPU(row) <-> PPU(column) Total");
        return;
    }

    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), new NodeHelperMulti(), PREFER_SRC_CONTEXT, MemcpyOperation::VECTOR_BW);
    PeerValueMatrix<double> bandwidthValuesWrite1(worldSize, worldSize, key + "_write1");
    PeerValueMatrix<double> bandwidthValuesWrite2(worldSize, worldSize, key + "_write2");
    PeerValueMatrix<double> bandwidthValuesTotal(worldSize, worldSize, key + "_total");

    for (const auto& pair : generateTestPairs(worldSize, gSettings.targetNumPairs)) {
        int srcDeviceId = pair.first;
        int peerDeviceId = pair.second;

        MultinodeDeviceBufferUnicast src1(size, srcDeviceId), src2(size, srcDeviceId);
        MultinodeDeviceBufferUnicast peer1(size, peerDeviceId), peer2(size, peerDeviceId);

        std::vector<const MemcpyBuffer*> srcNodes = {&src1, &peer2};
        std::vector<const MemcpyBuffer*> peerNodes = {&peer1, &src2};

        auto results = memcpyInstance.doMemcpyVector(srcNodes, peerNodes);
        bandwidthValuesWrite1.value(srcDeviceId, peerDeviceId) = results[0];
        bandwidthValuesWrite2.value(srcDeviceId, peerDeviceId) = results[1];
        bandwidthValuesTotal.value(srcDeviceId, peerDeviceId) = results[0] + results[1];
    }

    recordStability(bandwidthValuesTotal, memcpyInstance);
    output->addTestcaseResults(bandwidthValuesWrite1, "memcpy CE PPU(row) <-> PPU(column) Write1 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesWrite2, "memcpy CE PPU(row) <-> PPU(column) Write2 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesTotal, "memcpy CE PPU(row) <-> PPU(column) Total bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValuesTotal, true, "memcpy CE PPU(row) <-> PPU(column) Total");
}


// DtoD Read test - copy from dst to src (backwards) using src context
void MultinodeDeviceToDeviceReadSM::run(unsigned long long size, unsigned long long loopCount) {
    if (gSettings.isConcurrentPairSchedule()) {
        PeerValueMatrix<double> bandwidthValues(worldSize, worldSize, key);
        multinodeConcurrentPairHelper(size, loopCount, new MemcpyInitiatorSM(), true, bandwidthValues);
        output->addTestcaseResults(bandwidthValues, "memcpy SM PPU(row) -> PPU(column) bandwidth (GB/s)");
        output->displayUtilizationRatioMatrix(bandwidthValues, true, "memcpy SM PPU(row) -> PPU(column)");
        return;
    }

    PeerValueMatrix<double> bandwidthValues(worldSize, worldSize, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorSM(), new NodeHelperMulti(), PREFER_DST_CONTEXT);

    for (const auto& pair : generateTestPairs(worldSize, gSettings.targetNumPairs)) {
        int srcDeviceId = pair.first;
        int peerDeviceId = pair.second;

        MultinodeDeviceBufferUnicast srcNode(size, srcDeviceId);
        MultinodeDeviceBufferUnicast peerNode(size, peerDeviceId);

        // swap src and peer nodes, but use srcNodes (the copy's destination) context
        bandwidthValues.value(srcDeviceId, peerDeviceId) = memcpyInstance.doMemcpy(peerNode, srcNode);
    }

    recordStability(bandwidthValues, memcpyInstance);
    output->addTestcaseResults(bandwidthValues, "memcpy SM PPU(row) -> PPU(column) bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValues, true, "memcpy SM PPU(row) -> PPU(column)");
}

// DtoD Write test - copy from src to dst using src context
void MultinodeDeviceToDeviceWriteSM::run(unsigned long long size, unsigned long long loopCount) {
    if (gSettings.isConcurrentPairSchedule()) {
        PeerValueMatrix<double> bandwidthValues(worldSize, worldSize, key);
        multinodeConcurrentPairHelper(size, loopCount, new MemcpyInitiatorSM(), false, bandwidthValues);
        output->addTestcaseResults(bandwidthValues, "memcpy SM PPU(row) <- PPU(column) bandwidth (GB/s)");
        output->displayUtilizationRatioMatrix(bandwidthValues, true, "memcpy SM PPU(row) <- PPU(column)");
        return;
    }

    PeerValueMatrix<double> bandwidthValues(worldSize, worldSize, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorSM(), new NodeHelperMulti(), PREFER_SRC_CONTEXT);

    for (const auto& pair : generateTestPairs(worldSize, gSettings.targetNumPairs)) {
        int srcDeviceId = pair.first;
        int peerDeviceId = pair.second;

        MultinodeDeviceBufferUnicast srcNode(size, srcDeviceId);
        MultinodeDeviceBufferUnicast peerNode(size, peerDeviceId);

        bandwidthValues.value(srcDeviceId, peerDeviceId) = memcpyInstance.doMemcpy(srcNode, peerNode);
    }

    recordStability(bandwidthValues, memcpyInstance);
    output->addTestcaseResults(bandwidthValues, "memcpy SM PPU(row) <- PPU(column) bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValues, true, "memcpy SM PPU(row) <- PPU(column)");
}

// DtoD Bidir Read test - copy from dst to src (backwards) using src context
void MultinodeDeviceToDeviceBidirReadSM::run(unsigned long long size, unsigned long long loopCount) {
    if (gSettings.isConcurrentPairSchedule()) {
        PeerValueMatrix<double> bandwidthValuesRead1(worldSize, worldSize, key + "_read1");
        PeerValueMatrix<double> bandwidthValuesRead2(worldSize, worldSize, key + "_read2");
        PeerValueMatrix<double> bandwidthValuesTotal(worldSize, worldSize, key + "_total");
        multinodeConcurrentBidirPairHelper(size, loopCount, new MemcpyInitiatorSM(), true,
                                           bandwidthValuesRead1, bandwidthValuesRead2, bandwidthValuesTotal);
        output->addTestcaseResults(bandwidthValuesRead1, "memcpy SM PPU(row) <-> PPU(column) Read1 bandwidth (GB/s)");
        output->addTestcaseResults(bandwidthValuesRead2, "memcpy SM PPU(row) <-> PPU(column) Read2 bandwidth (GB/s)");
        output->addTestcaseResults(bandwidthValuesTotal, "memcpy SM PPU(row) <-> PPU(column) Total bandwidth (GB/s)");
        output->displayUtilizationRatioMatrix(bandwidthValuesTotal, true, "memcpy SM PPU(row) <-> PPU(column) Total");
        return;
    }

    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorSM(), new NodeHelperMulti(), PREFER_DST_CONTEXT, MemcpyOperation::VECTOR_BW);
    PeerValueMatrix<double> bandwidthValuesRead1(worldSize, worldSize, key + "_read1");
    PeerValueMatrix<double> bandwidthValuesRead2(worldSize, worldSize, key + "_read2");
    PeerValueMatrix<double> bandwidthValuesTotal(worldSize, worldSize, key + "_total");

    for (const auto& pair : generateTestPairs(worldSize, gSettings.targetNumPairs)) {
        int srcDeviceId = pair.first;
        int peerDeviceId = pair.second;

        MultinodeDeviceBufferUnicast src1(size, srcDeviceId), src2(size, srcDeviceId);
        MultinodeDeviceBufferUnicast peer1(size, peerDeviceId), peer2(size, peerDeviceId);

        // swap src and peer nodes, but use srcNodes (the copy's destination) context
        std::vector<const MemcpyBuffer*> srcNodes = {&peer1, &src2};
        std::vector<const MemcpyBuffer*> peerNodes = {&src1, &peer2};

        auto results = memcpyInstance.doMemcpyVector(srcNodes, peerNodes);
        bandwidthValuesRead1.value(srcDeviceId, peerDeviceId) = results[0];
        bandwidthValuesRead2.value(srcDeviceId, peerDeviceId) = results[1];
        bandwidthValuesTotal.value(srcDeviceId, peerDeviceId) = results[0] + results[1];
    }

    recordStability(bandwidthValuesTotal, memcpyInstance);
    output->addTestcaseResults(bandwidthValuesRead1, "memcpy SM PPU(row) <-> PPU(column) Read1 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesRead2, "memcpy SM PPU(row) <-> PPU(column) Read2 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesTotal, "memcpy SM PPU(row) <-> PPU(column) Total bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValuesTotal, true, "memcpy SM PPU(row) <-> PPU(column) Total");
}

// DtoD Bidir Write test - copy from src to dst using src context
void MultinodeDeviceToDeviceBidirWriteSM::run(unsigned long long size, unsigned long long loopCount) {
    if (gSettings.isConcurrentPairSchedule()) {
        PeerValueMatrix<double> bandwidthValuesWrite1(worldSize, worldSize, key + "_write1");
        PeerValueMatrix<double> bandwidthValuesWrite2(worldSize, worldSize, key + "_write2");
        PeerValueMatrix<double> bandwidthValuesTotal(worldSize, worldSize, key + "_total");
        multinodeConcurrentBidirPairHelper(size, loopCount, new MemcpyInitiatorSM(), false,
                                           bandwidthValuesWrite1, bandwidthValuesWrite2, bandwidthValuesTotal);
        output->addTestcaseResults(bandwidthValuesWrite1, "memcpy SM PPU(row) <-> PPU(column) Write1 bandwidth (GB/s)");
        output->addTestcaseResults(bandwidthValuesWrite2, "memcpy SM PPU(row) <-> PPU(column) Write2 bandwidth (GB/s)");
        output->addTestcaseResults(bandwidthValuesTotal, "memcpy SM PPU(row) <-> PPU(column) Total bandwidth (GB/s)");
        output->displayUtilizationRatioMatrix(bandwidthValuesTotal, true, "memcpy SM PPU(row) <-> PPU(column) Total");
        return;
    }

    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorSM(), new NodeHelperMulti(), PREFER_SRC_CONTEXT, MemcpyOperation::VECTOR_BW);
    PeerValueMatrix<double> bandwidthValuesWrite1(worldSize, worldSize, key + "_write1");
    PeerValueMatrix<double> bandwidthValuesWrite2(worldSize, worldSize, key + "_write2");
    PeerValueMatrix<double> bandwidthValuesTotal(worldSize, worldSize, key + "_total");

    for (const auto& pair : generateTestPairs(worldSize, gSettings.targetNumPairs)) {
        int srcDeviceId = pair.first;
        int peerDeviceId = pair.second;

        MultinodeDeviceBufferUnicast src1(size, srcDeviceId), src2(size, srcDeviceId);
        MultinodeDeviceBufferUnicast peer1(size, peerDeviceId), peer2(size, peerDeviceId);

        std::vector<const MemcpyBuffer*> srcNodes = {&src1, &peer2};
        std::vector<const MemcpyBuffer*> peerNodes = {&peer1, &src2};

        auto results = memcpyInstance.doMemcpyVector(srcNodes, peerNodes);
        bandwidthValuesWrite1.value(srcDeviceId, peerDeviceId) = results[0];
        bandwidthValuesWrite2.value(srcDeviceId, peerDeviceId) = results[1];
        bandwidthValuesTotal.value(srcDeviceId, peerDeviceId) = results[0] + results[1];
    }

    recordStability(bandwidthValuesTotal, memcpyInstance);
    output->addTestcaseResults(bandwidthValuesWrite1, "memcpy SM PPU(row) <-> PPU(column) Write1 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesWrite2, "memcpy SM PPU(row) <-> PPU(column) Write2 bandwidth (GB/s)");
    output->addTestcaseResults(bandwidthValuesTotal, "memcpy SM PPU(row) <-> PPU(column) Total bandwidth (GB/s)");
    output->displayUtilizationRatioMatrix(bandwidthValuesTotal, true, "memcpy SM PPU(row) <-> PPU(column) Total");
}

void MultinodeAllToOneWriteSM::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, worldSize, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorSM(), new NodeHelperMulti(), PREFER_SRC_CONTEXT, MemcpyOperation::SUM_BW);

    for (int dstDeviceId = 0; dstDeviceId < worldSize; dstDeviceId++) {
        std::vector<const MemcpyBuffer*> srcNodes;
        std::vector<const MemcpyBuffer*> dstNodes;

        for (int srcDeviceId = 0; srcDeviceId < worldSize; srcDeviceId++) {
            if (dstDeviceId == srcDeviceId) {
                continue;
            }

            srcNodes.push_back(new MultinodeDeviceBufferLocal(size, srcDeviceId));
            dstNodes.push_back(new MultinodeDeviceBufferUnicast(size, dstDeviceId));
        }

        bandwidthValues.value(0, dstDeviceId) = memcpyInstance.doMemcpy(srcNodes, dstNodes);

        for (auto node : dstNodes) {
            delete node;
        }
        for (auto node : srcNodes) {
            delete node;
        }
    }

    recordStability(bandwidthValues, memcpyInstance);
    output->addTestcaseResults(bandwidthValues, "memcpy SM All PPUs -> PPU(column) total bandwidth (GB/s)");
}

void MultinodeAllFromOneReadSM::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, worldSize, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorSM(), new NodeHelperMulti(), PREFER_DST_CONTEXT, MemcpyOperation::SUM_BW);

    for (int srcDeviceId = 0; srcDeviceId < worldSize; srcDeviceId++) {
        std::vector<const MemcpyBuffer*> srcNodes;
        std::vector<const MemcpyBuffer*> dstNodes;

        for (int dstDeviceId = 0; dstDeviceId < worldSize; dstDeviceId++) {
            if (dstDeviceId == srcDeviceId) {
                continue;
            }

            srcNodes.push_back(new MultinodeDeviceBufferUnicast(size, srcDeviceId));
            dstNodes.push_back(new MultinodeDeviceBufferLocal(size, dstDeviceId));
        }

        bandwidthValues.value(0, srcDeviceId) = memcpyInstance.doMemcpy(srcNodes, dstNodes);

        for (auto node : dstNodes) {
            delete node;
        }
        for (auto node : srcNodes) {
            delete node;
        }
    }

    recordStability(bandwidthValues, memcpyInstance);
    output->addTestcaseResults(bandwidthValues, "memcpy SM All PPUs <- PPU(column) total bandwidth (GB/s)");
}

void MultinodeBroadcastOneToAllSM::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, worldSize, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorMulticastWrite(), new NodeHelperMulti(), PREFER_DST_CONTEXT, MemcpyOperation::SUM_BW);

    for (int dstDeviceId = 0; dstDeviceId < worldSize; dstDeviceId++) {
        std::vector<const MemcpyBuffer*> srcNodes;
        std::vector<const MemcpyBuffer*> dstNodes;

        srcNodes.push_back(new MultinodeDeviceBufferLocal(size, dstDeviceId));
        dstNodes.push_back(new MultinodeDeviceBufferMulticast(size, dstDeviceId));

        bandwidthValues.value(0, dstDeviceId) = memcpyInstance.doMemcpy(srcNodes, dstNodes);

        for (auto node : dstNodes) {
            delete node;
        }
        for (auto node : srcNodes) {
            delete node;
        }
    }

    recordStability(bandwidthValues, memcpyInstance);
    output->addTestcaseResults(bandwidthValues, "multicast SM PPU(column) -> All PPUs total bandwidth (GB/s)");
}

void MultinodeBroadcastAllToAllSM::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(1, 1, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorMulticastWrite(), new NodeHelperMulti(), PREFER_DST_CONTEXT, MemcpyOperation::SUM_BW);
    std::vector<const MemcpyBuffer*> srcNodes;
    std::vector<const MemcpyBuffer*> dstNodes;

    for (int dstDeviceId = 0; dstDeviceId < worldSize; dstDeviceId++) {
        srcNodes.push_back(new MultinodeDeviceBufferLocal(size, dstDeviceId));
        dstNodes.push_back(new MultinodeDeviceBufferMulticast(size, dstDeviceId));
    }

    bandwidthValues.value(0, 0) = memcpyInstance.doMemcpy(srcNodes, dstNodes);

    for (auto node : dstNodes) {
        delete node;
    }
    for (auto node : srcNodes) {
        delete node;
    }

    recordStability(bandwidthValues, memcpyInstance);
    output->addTestcaseResults(bandwidthValues, "multicast SM All -> All PPUs total bandwidth (GB/s)");
}

void MultinodeBisectWriteCE::run(unsigned long long size, unsigned long long loopCount) {
    PeerValueMatrix<double> bandwidthValues(worldSize, 1, key);
    MemcpyOperation memcpyInstance(loopCount, new MemcpyInitiatorCE(), new NodeHelperMulti(), PREFER_SRC_CONTEXT, MemcpyOperation::VECTOR_BW);
    std::vector<std::string> rowLabels;
    std::vector<const MemcpyBuffer*> srcNodes, dstNodes;

    for (int i = 0; i < worldSize; i++) {
        int peer = (i + worldSize / 2) % worldSize;
        srcNodes.push_back(new MultinodeDeviceBufferUnicast(size, i));
        dstNodes.push_back(new MultinodeDeviceBufferUnicast(size, peer));

        std::stringstream s;
        s << getPaddedProcessId(i) << "->" << getPaddedProcessId(peer);
        rowLabels.push_back(s.str());
    }

    auto results = memcpyInstance.doMemcpyVector(dstNodes, srcNodes);

    for (int i = 0; i < results.size(); i++) {
        bandwidthValues.value(i, 0) = results[i];
    }
    bandwidthValues.setRowLabels(rowLabels);

    for (auto node : dstNodes) {
        delete node;
    }
    for (auto node : srcNodes) {
        delete node;
    }

    output->addTestcaseResults(bandwidthValues, "Bisect benchmarking, simultaneous write CE BW");
}

#endif
