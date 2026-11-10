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

#ifdef MULTINODE
#include <algorithm>
#include <mpi.h>
#include <random>
#include <set>
#include <unistd.h>

#include "kernels.cuh"
#include "multinode_memcpy.h"
#include "output.h"
#include "verify_utils.h"
#include <array>

MultinodeMemoryAllocation::MultinodeMemoryAllocation(size_t bufferSize, int MPI_rank): bufferSize(bufferSize), MPI_rank(MPI_rank) {
    hggcSetDevice(localDevice);
}

static HGresult MPIstreamSyncHelper(HGstream stream) {
    HGresult err = HGGC_ERROR_NOT_READY;
    int flag;
    while (err == HGGC_ERROR_NOT_READY) {
        err = hgStreamQuery(stream);
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, MPI_STATUS_IGNORE);
    }
    return err;
}

HGresult MultinodeMemoryAllocation::streamSynchronizeWrapper(HGstream stream) const {
    return MPIstreamSyncHelper(stream);
}

MultinodeMemoryAllocationUnicast::MultinodeMemoryAllocationUnicast(size_t bufferSize, int MPI_rank): MultinodeMemoryAllocation(bufferSize, MPI_rank) {
    handleType = HG_MEM_HANDLE_TYPE_FABRIC;
    prop.type = HG_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = HG_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = localDevice;
    prop.requestedHandleTypes = handleType;

    size_t granularity = 0;
    HG_ASSERT(hgMemGetAllocationGranularity(&granularity, &prop, HG_MEM_ALLOC_GRANULARITY_RECOMMENDED));

    roundedUpAllocationSize = ROUND_UP(bufferSize, granularity);

    if (MPI_rank == worldRank) {
        // Allocate the memory
        HG_ASSERT(hgMemCreate(&handle, roundedUpAllocationSize, &prop, 0 /*flags*/));

        // Export the allocation to the importing process
        HG_ASSERT(hgMemExportToShareableHandle(&fh, handle, handleType, 0 /*flags*/));
    }

    MPI_Bcast(&fh, sizeof(fh), MPI_BYTE, MPI_rank, MPI_COMM_WORLD);

    if (MPI_rank != worldRank) {
        HG_ASSERT(hgMemImportFromShareableHandle(&handle, (void *)&fh, handleType));
    }

    // Map the memory
    HG_ASSERT(hgMemAddressReserve((HGdeviceptr *) &buffer, roundedUpAllocationSize, 0, 0 /*baseVA*/, 0 /*flags*/));

    HG_ASSERT(hgMemMap((HGdeviceptr) buffer, roundedUpAllocationSize, 0 /*offset*/, handle, 0 /*flags*/));
    desc.location.type = HG_MEM_LOCATION_TYPE_DEVICE;
    desc.location.id = localDevice;
    desc.flags = HG_MEM_ACCESS_FLAGS_PROT_READWRITE;
    HG_ASSERT(hgMemSetAccess((HGdeviceptr) buffer, roundedUpAllocationSize, &desc, 1 /*count*/));

    // Make sure that everyone is done with mapping the fabric allocation
    ASSERT(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS);
}

MultinodeMemoryAllocationUnicast::~MultinodeMemoryAllocationUnicast() {
    // Make sure that everyone is done using the memory
    ASSERT(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS);

    HG_ASSERT(hgMemUnmap((HGdeviceptr) buffer, roundedUpAllocationSize));
    HG_ASSERT(hgMemRelease(handle));
    HG_ASSERT(hgMemAddressFree((HGdeviceptr) buffer, roundedUpAllocationSize));
}

MultinodeMemoryAllocationMulticast::MultinodeMemoryAllocationMulticast(size_t bufferSize, int MPI_rank): MultinodeMemoryAllocation(bufferSize, MPI_rank) {
    handleType = HG_MEM_HANDLE_TYPE_FABRIC;
    multicastProp.numDevices = worldSize;
    multicastProp.handleTypes = handleType;
    size_t gran;
    HG_ASSERT(hgMulticastGetGranularity(&gran, &multicastProp, HG_MULTICAST_GRANULARITY_RECOMMENDED));
    roundedUpAllocationSize = ROUND_UP(bufferSize, gran);
    multicastProp.size = roundedUpAllocationSize;

    if (MPI_rank == worldRank) {
        // Allocate the memory
        HG_ASSERT(hgMulticastCreate(&multicastHandle, &multicastProp));

        // Export the allocation to the importing process
        HG_ASSERT(hgMemExportToShareableHandle(&fh, multicastHandle, handleType, 0 /*flags*/));
    }

    MPI_Bcast(&fh, sizeof(fh), MPI_BYTE, MPI_rank, MPI_COMM_WORLD);

    if (MPI_rank != worldRank) {
        HG_ASSERT(hgMemImportFromShareableHandle(&multicastHandle, (void *)&fh, handleType));
    }

    HGdevice dev;
    HG_ASSERT(hgDeviceGet(&dev, localDevice));
    HG_ASSERT(hgMulticastAddDevice(multicastHandle, dev));

    // Ensure all devices in this process are added BEFORE binding mem on any device
    MPI_Barrier(MPI_COMM_WORLD);

    // Allocate the memory (same as unicast) and bind to MC handle
    HGmemAllocationProp prop = {};
    prop.type = HG_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = HG_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = localDevice;
    prop.requestedHandleTypes = handleType;
    HG_ASSERT(hgMemCreate(&handle, roundedUpAllocationSize, &prop, 0 /*flags*/));
    HG_ASSERT(hgMulticastBindMem(multicastHandle, 0, handle, 0, roundedUpAllocationSize, 0));

    // Map the memory
    HG_ASSERT(hgMemAddressReserve((HGdeviceptr *) &buffer, roundedUpAllocationSize, 0, 0 /*baseVA*/, 0 /*flags*/));

    HG_ASSERT(hgMemMap((HGdeviceptr) buffer, roundedUpAllocationSize, 0 /*offset*/, multicastHandle, 0 /*flags*/));
    desc.location.type = HG_MEM_LOCATION_TYPE_DEVICE;
    desc.location.id = localDevice;
    desc.flags = HG_MEM_ACCESS_FLAGS_PROT_READWRITE;
    HG_ASSERT(hgMemSetAccess((HGdeviceptr) buffer, roundedUpAllocationSize, &desc, 1 /*count*/));

    // Make sure that everyone is done with mapping the fabric allocation
    MPI_Barrier(MPI_COMM_WORLD);
}

MultinodeMemoryAllocationMulticast::~MultinodeMemoryAllocationMulticast() {
    // Make sure that everyone is done using the memory
    MPI_Barrier(MPI_COMM_WORLD);

    HGdevice dev;
    HG_ASSERT(hgDeviceGet(&dev, localDevice));
    HG_ASSERT(hgMulticastUnbind(multicastHandle, dev, 0, roundedUpAllocationSize));
    HG_ASSERT(hgMemRelease(handle));

    HG_ASSERT(hgMemUnmap((HGdeviceptr) buffer, roundedUpAllocationSize));
    HG_ASSERT(hgMemRelease(multicastHandle));
    HG_ASSERT(hgMemAddressFree((HGdeviceptr) buffer, roundedUpAllocationSize));
}

MultinodeDeviceBuffer::MultinodeDeviceBuffer(size_t bufferSize, int MPI_rank):
    MemcpyBuffer(bufferSize),  MPI_rank(MPI_rank) {
    HG_ASSERT(hgDevicePrimaryCtxRetain(&primaryCtx, localDevice));
}

MultinodeDeviceBuffer::~MultinodeDeviceBuffer() {
    // Runs after the derived class has freed its allocation, so the context outlives every use of it.
    HG_ASSERT(hgDevicePrimaryCtxRelease(localDevice));
}

int MultinodeDeviceBuffer::getBufferIdx() const {
    // only single-PPU supported for now
    return 0;
}

std::string MultinodeDeviceBuffer::getBufferString() const {
    return "Multinode node " + std::to_string(MPI_rank);
}

HGcontext MultinodeDeviceBuffer::getPrimaryCtx() const {
    return primaryCtx;
}

int MultinodeDeviceBuffer::getMPIRank() const {
    return MPI_rank;
}

bool MultinodeDeviceBuffer::isDeviceBuffer() const {
    return true;
}

MultinodeDeviceBufferUnicast::MultinodeDeviceBufferUnicast(size_t bufferSize, int MPI_rank):
    MultinodeDeviceBuffer(bufferSize, MPI_rank),
    MemoryAllocation(bufferSize, MPI_rank) {
    buffer = MemoryAllocation.getBuffer();
}

MultinodeDeviceBufferMulticast::MultinodeDeviceBufferMulticast(size_t bufferSize, int MPI_rank):
    MultinodeDeviceBuffer(bufferSize, MPI_rank),
    MemoryAllocation(bufferSize, MPI_rank) {
    buffer = MemoryAllocation.getBuffer();
}

MultinodeDeviceBufferLocal::MultinodeDeviceBufferLocal(size_t bufferSize, int MPI_rank):
    MultinodeDeviceBuffer(bufferSize, MPI_rank) {
    buffer = nullptr;
    if (worldRank == MPI_rank) {
        HG_ASSERT(hgDevicePrimaryCtxRetain(&primaryCtx, localDevice));
        HG_ASSERT(hgCtxSetCurrent(primaryCtx));
        if (bufferSize) {
            HG_ASSERT(hgMemAlloc((HGdeviceptr*)&buffer, bufferSize));
        }
    }
}

MultinodeDeviceBufferLocal::~MultinodeDeviceBufferLocal() {
    if (buffer) {
        HG_ASSERT(hgCtxSetCurrent(primaryCtx));
        HG_ASSERT(hgMemFree((HGdeviceptr)buffer));
        HG_ASSERT(hgDevicePrimaryCtxRelease(localDevice));
    }
}

// Helper function to generate test pairs using NodeHelperMulti sampling with target number of pairs
std::vector<std::pair<int, int>> generateTestPairs(int worldSize, long long targetNumPairs) {
    // Members are static, we only need to do the pair-wise sampling once.
    static std::vector<std::pair<int, int>> allValidPairs;  // All possible (src != dst) PPU pairs
    static std::vector<std::pair<int, int>> selectedPairs;  // Subset of pairs chosen via sampling
    static bool pairsCalculated = false;  // Ensure the chosen pairs are computed once per run

    // Handle special cases
    if (targetNumPairs == 0) {
        return {};
    }
    if (targetNumPairs == -1) {
        targetNumPairs = ((worldSize) * (worldSize - 1));
    }

    bool useDecisionSampling = targetNumPairs < ((worldSize) * (worldSize - 1));

    // If pairs were already calculated and we're not doing decision sampling, return all pairs
    if (pairsCalculated && !useDecisionSampling) {
        return allValidPairs;
    }

    // If pairs were already calculated and we are doing decision sampling, return selected pairs.
    // Currently, we only need to calculate the pairs once for all tests.
    if (pairsCalculated && useDecisionSampling && !selectedPairs.empty()) {
        return selectedPairs;
    }

    if (worldRank == 0) {
        std::vector<bool> ppuIncluded(worldSize, false);

        if (!pairsCalculated) {
            // Generate all valid pairs
            for (int srcDeviceId = 0; srcDeviceId < worldSize; srcDeviceId++) {
                for (int peerDeviceId = 0; peerDeviceId < worldSize; peerDeviceId++) {
                    if (peerDeviceId == srcDeviceId) {
                        continue;
                    }
                    allValidPairs.push_back({srcDeviceId, peerDeviceId});
                }
            }

            // Use fixed seed for reproducible results
            std::mt19937 gen(0xBAADF00D);
            if (gSettings.targetNumPairsSpecified) {
                std::shuffle(allValidPairs.begin(), allValidPairs.end(), gen);
            }

            if (useDecisionSampling) {
                selectedPairs.clear();
                std::set<std::pair<int, int>> selectedPairsSet;

                std::vector<int> ppuOrder;
                for (int i = 0; i < worldSize; i++) {
                    ppuOrder.push_back(i);
                }
                std::shuffle(ppuOrder.begin(), ppuOrder.end(), gen);

                // First pass: try to include each PPU at least once
                for (int ppu : ppuOrder) {
                    if (selectedPairsSet.size() >= static_cast<size_t>(targetNumPairs)) {
                        break;
                    }
                    if (ppuIncluded[ppu]) {
                        continue;
                    }
                    // Find a pair that includes this PPU
                    for (const auto &pair : allValidPairs) {
                        if ((pair.first == ppu || pair.second == ppu) &&
                            selectedPairsSet.size() < static_cast<size_t>(targetNumPairs)) {
                            if (selectedPairsSet.find(pair) == selectedPairsSet.end()) {
                                selectedPairsSet.insert(pair);
                                selectedPairs.push_back(pair);
                                ppuIncluded[pair.first] = true;
                                ppuIncluded[pair.second] = true;
                                break;  // Found one pair for this PPU, move to next
                            }
                        }
                    }
                }

                // Second pass: fill remaining slots with random pairs
                if (selectedPairsSet.size() < static_cast<size_t>(targetNumPairs)) {
                    // Create a list of remaining pairs (excluding already selected ones)
                    std::vector<std::pair<int, int>> remainingPairs;
                    for (const auto &pair : allValidPairs) {
                        if (selectedPairsSet.find(pair) == selectedPairsSet.end()) {
                            remainingPairs.push_back(pair);
                        }
                    }

                    // Shuffle remaining pairs and add them
                    if (!remainingPairs.empty()) {
                        std::shuffle(remainingPairs.begin(), remainingPairs.end(), gen);

                        int remainingSlots = targetNumPairs - selectedPairsSet.size();
                        int pairsToAdd = std::min(remainingSlots, static_cast<int>(remainingPairs.size()));

                        for (int i = 0; i < pairsToAdd; i++) {
                            selectedPairsSet.insert(remainingPairs[i]);
                            selectedPairs.push_back(remainingPairs[i]);
                        }
                    }
                }
            }
            pairsCalculated = true;
        }
    }

    ASSERT(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS);
    pairsCalculated = true;

    std::vector<std::pair<int, int>> finalPairs;
    if (worldRank == 0) {
        if (useDecisionSampling) {
            finalPairs = selectedPairs;
        } else {
            finalPairs = allValidPairs;
        }
    }

    // Broadcast the size first
    long pair_vector_size = finalPairs.size();
    MPI_Bcast(&pair_vector_size, 1, MPI_LONG, 0, MPI_COMM_WORLD);

    // Serialize pairs to flat integer array for safe MPI communication
    std::vector<int> serializedPairs;
    if (worldRank == 0) {
        serializedPairs.reserve(finalPairs.size() * 2);
        for (const auto& pair : finalPairs) {
            serializedPairs.push_back(pair.first);
            serializedPairs.push_back(pair.second);
        }
    } else {
        serializedPairs.resize(pair_vector_size * 2);
        finalPairs.resize(pair_vector_size);
    }

    // Broadcast the serialized data
    MPI_Bcast(serializedPairs.data(), serializedPairs.size(), MPI_INT, 0, MPI_COMM_WORLD);

    // Deserialize back to pairs on non-rank-0 processes
    if (worldRank != 0) {
        for (size_t i = 0; i < finalPairs.size(); ++i) {
            finalPairs[i] = {serializedPairs[i * 2], serializedPairs[i * 2 + 1]};
        }
        if (useDecisionSampling) {
            selectedPairs = finalPairs;
        } else {
            allValidPairs = finalPairs;
        }
    }

    return finalPairs;
}

NodeHelperMulti::NodeHelperMulti(bool localRelease) : localRelease(localRelease), blockingVarDeviceAllocation(sizeof(*blockingVarDevice), 0)  {
    HG_ASSERT(hgMemHostAlloc((void **)&blockingVarHost, sizeof(*blockingVarHost), HG_MEMHOSTALLOC_PORTABLE));
    blockingVarDevice = (volatile int*) blockingVarDeviceAllocation.getBuffer();
    if (localRelease) {
        HG_ASSERT(hgMemHostAlloc((void **)&spinTimeoutHost, sizeof(*spinTimeoutHost), HG_MEMHOSTALLOC_PORTABLE));
        *spinTimeoutHost = 0;
    }
}

NodeHelperMulti::~NodeHelperMulti() {
    if (localRelease) errorContext.clear();
    if (spinTimeoutHost != nullptr) {
        HG_ASSERT(hgMemFreeHost((void*)spinTimeoutHost));
    }
    HG_ASSERT(hgMemFreeHost((void*)blockingVarHost));
}

MemcpyDispatchInfo NodeHelperMulti::dispatchMemcpy(const std::vector<const MemcpyBuffer*> &srcNodesUnfiltered, const std::vector<const MemcpyBuffer*> &dstNodesUnfiltered,
                                                   P2PType p2pType, ContextPreference ctxPreference) {
    std::vector<int> ranksUnfiltered(srcNodesUnfiltered.size(), -1);
    std::vector<HGcontext> contextsUnfiltered(srcNodesUnfiltered.size());
    std::vector<const MemcpyBuffer*> srcNodes;
    std::vector<const MemcpyBuffer*> dstNodes;
    std::vector<HGcontext> contexts;

    for (int i = 0; i < srcNodesUnfiltered.size(); i++) {
        // prefer source context
        // determine which ranks executes given operation
        if (ctxPreference == PREFER_SRC_CONTEXT && srcNodesUnfiltered[i]->getPrimaryCtx() != nullptr) {
            contextsUnfiltered[i] = srcNodesUnfiltered[i]->getPrimaryCtx();
            ranksUnfiltered[i] = srcNodesUnfiltered[i]->getMPIRank();
        } else if (dstNodesUnfiltered[i]->getPrimaryCtx() != nullptr) {
            contextsUnfiltered[i] = dstNodesUnfiltered[i]->getPrimaryCtx();
            ranksUnfiltered[i] = dstNodesUnfiltered[i]->getMPIRank();
        }
    }

    for (int i = 0; i < srcNodesUnfiltered.size(); i++) {
        if (ranksUnfiltered[i] == worldRank) {
            srcNodes.push_back(srcNodesUnfiltered[i]);
            dstNodes.push_back(dstNodesUnfiltered[i]);
            contexts.push_back(contextsUnfiltered[i]);
        }
    }

    // Don't crash if there are no memcopies to do
    if (ranksUnfiltered.size() > 0) {
        rankOfFirstMemcpy = ranksUnfiltered[0];
    }

    if (localRelease) {
        phaseSrcBuffers = srcNodesUnfiltered;
        phaseDstBuffers = dstNodesUnfiltered;
        if (gSettings.verbose && worldRank == 0) {
            std::ostringstream description;
            description << "Phase " << phaseNumber << "/" << phaseCount << " " << currentTestcaseName
                        << ": " << srcNodesUnfiltered.size() << " transports\n";
            for (size_t i = 0; i < srcNodesUnfiltered.size(); i++) {
                const int src = srcNodesUnfiltered[i]->getMPIRank();
                const int dst = dstNodesUnfiltered[i]->getMPIRank();
                description << "  Transport " << i << ": " << src << " -> " << dst
                            << ", executor=" << ranksUnfiltered[i] << ", src=" << ppuHostByRank[src] << "/"
                            << ppuBDFByRank[src] << ", dst=" << ppuHostByRank[dst] << "/" << ppuBDFByRank[dst] << '\n';
            }
            output->recordPhaseDiagnostics(description.str());
        }
    }
    return MemcpyDispatchInfo(p2pType, gSettings.isSingleDie, contexts, srcNodes, dstNodes, ranksUnfiltered);
}

double NodeHelperMulti::calculateTotalBandwidth(double totalTime, double totalSize, size_t loopCount) {
    double totalMax = 0;
    MPI_Allreduce(&totalTime, &totalMax, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
    totalTime = totalMax;

    double totalSum = 0;
    MPI_Allreduce(&totalSize, &totalSum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    totalSize = totalSum;

    return (totalSize * loopCount * 1000ull * 1000ull) / totalTime;
}

double NodeHelperMulti::calculateSumBandwidth(std::vector<PerformanceStatistic> &bandwidthStats) {
    double sum = 0.0;
    for (auto stat : bandwidthStats) {
        sum += stat.returnAppropriateMetric() * 1e-9;
    }
    // Calculate total BW sum across all nodes and memcopies
    double totalSum = 0;
    MPI_Allreduce(&sum, &totalSum, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    return totalSum;
}

double NodeHelperMulti::calculateFirstBandwidth(std::vector<PerformanceStatistic> &bandwidthStats) {
    // Broadcast bandwidth of "first" memcopy to other nodes
    double retval = 0;
    if (worldRank == rankOfFirstMemcpy) {
        retval = bandwidthStats[0].returnAppropriateMetric() * 1e-9;
    }
    MPI_Bcast(&retval, 1, MPI_DOUBLE, rankOfFirstMemcpy, MPI_COMM_WORLD);
    return retval;
}

std::vector<double> NodeHelperMulti::calculateVectorBandwidth(std::vector<double> &results, std::vector<int> originalRanks) {
    std::vector<double> retval;
    int current_local_elem = 0;
    for (int i = 0; i < originalRanks.size(); i++) {
        double tmp = 0;
        if (worldRank == originalRanks[i]) {
            tmp = results[current_local_elem];
            current_local_elem++;
        }
        MPI_Bcast(&tmp, 1, MPI_DOUBLE, originalRanks[i], MPI_COMM_WORLD);
        retval.push_back(tmp);
    }
    return retval;
}

void NodeHelperMulti::synchronizeProcess() {
    ASSERT(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS);
}

HGresult NodeHelperMulti::streamSynchronizeWrapper(HGstream stream) const {
    return MPIstreamSyncHelper(stream);
}

void NodeHelperMulti::streamBlockerReset() {
    *blockingVarHost = 0;
    if (localRelease) {
        *spinTimeoutHost = 0;
        deferred.reports.clear();
        deferred.verboseReport.clear();
        deferred.context = std::to_string(phaseNumber) + "/" + std::to_string(phaseCount) +
                           ", sample " + std::to_string(++sampleNumber);
        deferred.dumpTag = "_pid" + std::to_string(getpid()) + "_phase" + std::to_string(phaseNumber) +
                           "_sample" + std::to_string(sampleNumber);
    } else {
        HG_ASSERT(hgMemsetD32((HGdeviceptr) blockingVarDevice, 0, 1));
    }
}

void NodeHelperMulti::streamBlockerRelease() {
    if (localRelease) {
        synchronizeProcess();
    }
    *blockingVarHost = 1;
}

void NodeHelperMulti::streamBlockerBlock(HGstream mainstream, HGstream die0stream, HGstream die1stream) {
    if (localRelease) {
        HGstream die1BlockedStream = gSettings.isSingleDie ? nullptr : die1stream;
        HG_ASSERT(spinKernelReportTimeout(blockingVarHost, spinTimeoutHost, mainstream, die0stream, die1BlockedStream));
        return;
    }
    // MPI rank ranks[0] is released by blockingVar, writes to blockingVarDevice, releasing other ranks
    HG_ASSERT(spinKernelMultistage((worldRank == rankOfFirstMemcpy) ? blockingVarHost : nullptr, blockingVarDevice, mainstream, die0stream, die1stream));
}

bool NodeHelperMulti::streamBlockerTimedOut() {
    if (!localRelease) {
        return false;
    }
    int timedOut = (*spinTimeoutHost != 0) ? 1 : 0;
    if (timedOut) {
        deferred.reports.push_back("Spin release timeout: testcase=" + currentTestcaseName + ", phase=" +
                                   deferred.context + ", rank=" + std::to_string(worldRank) +
                                   ", host=" + ppuHostByRank[worldRank] + ", BDF=" + ppuBDFByRank[worldRank] + "\n");
    }
    ASSERT(MPI_Allreduce(MPI_IN_PLACE, &timedOut, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD) == MPI_SUCCESS);
    return timedOut != 0;
}

DeferredVerification *NodeHelperMulti::deferredVerification() {
    return localRelease ? &deferred : nullptr;
}

void NodeHelperMulti::beginPhase(size_t number, size_t count) {
    phaseNumber = number;
    phaseCount = count;
    sampleNumber = 0;
    errorContext = ", testcase=" + currentTestcaseName + ", phase=" + std::to_string(number) + "/" + std::to_string(count);
}

void NodeHelperMulti::verifyPhaseBuffers(MemcpyDispatchInfo &info) {
    size_t localIndex = 0;
    for (size_t i = 0; i < info.originalRanks.size(); i++) {
        unsigned long long bytes = 0;
        if (worldRank == info.originalRanks[i]) bytes = info.adjustedCopySizes[localIndex++];
        ASSERT(MPI_Bcast(&bytes, 1, MPI_UNSIGNED_LONG_LONG, info.originalRanks[i], MPI_COMM_WORLD) == MPI_SUCCESS);
        const auto *src = phaseSrcBuffers[i];
        const auto *dst = phaseDstBuffers[i];
        if (src->getMPIRank() == worldRank) {
            verifyMemcpyBuffer(src, dst, true, static_cast<size_t>(bytes), info.nodeHelper);
        }
        if (dst->getMPIRank() == worldRank) {
            verifyMemcpyBuffer(src, dst, false, static_cast<size_t>(bytes), info.nodeHelper);
        }
    }
}

static std::string gatherPhaseText(const std::string &local) {
    std::string result;
    std::array<char, 65536> chunk;
    for (int rank = 0; rank < worldSize; rank++) {
        unsigned long long length = worldRank == rank ? local.size() : 0;
        ASSERT(MPI_Bcast(&length, 1, MPI_UNSIGNED_LONG_LONG, rank, MPI_COMM_WORLD) == MPI_SUCCESS);
        for (unsigned long long offset = 0; offset < length; offset += chunk.size()) {
            const int count = static_cast<int>(std::min<unsigned long long>(chunk.size(), length - offset));
            if (worldRank == rank) std::copy_n(local.data() + offset, count, chunk.data());
            ASSERT(MPI_Bcast(chunk.data(), count, MPI_CHAR, rank, MPI_COMM_WORLD) == MPI_SUCCESS);
            if (worldRank == 0) result.append(chunk.data(), count);
        }
    }
    return result;
}

PhaseVerificationFailures NodeHelperMulti::gatherPhaseVerificationFailures() {
    PhaseVerificationFailures failures;
    const int localFailures = static_cast<int>(deferred.reports.size());
    failures.failuresByRank.resize(worldSize);
    ASSERT(MPI_Allgather(&localFailures, 1, MPI_INT, failures.failuresByRank.data(), 1, MPI_INT, MPI_COMM_WORLD) == MPI_SUCCESS);
    if (gSettings.verbose) {
        const std::string verbose = gatherPhaseText(deferred.verboseReport);
        if (worldRank == 0) output->recordPhaseDiagnostics(verbose);
    }
    if (std::any_of(failures.failuresByRank.begin(), failures.failuresByRank.end(), [](int count) { return count != 0; })) {
        std::string localReports;
        for (const auto &report : deferred.reports) localReports += report + "\n";
        failures.reports = gatherPhaseText(localReports);
    }
    return failures;
}

void NodeHelperMulti::reportPhaseSample(MemcpyDispatchInfo &info, const std::vector<double> &bandwidths) {
    size_t localIndex = 0;
    std::ostringstream text;
    for (size_t i = 0; i < info.originalRanks.size(); i++) {
        double value = 0;
        if (worldRank == info.originalRanks[i]) value = bandwidths[localIndex++];
        ASSERT(MPI_Bcast(&value, 1, MPI_DOUBLE, info.originalRanks[i], MPI_COMM_WORLD) == MPI_SUCCESS);
        text << "  Sample " << sampleNumber << ": " << phaseSrcBuffers[i]->getBufferString() << " -> "
             << phaseDstBuffers[i]->getBufferString() << ": " << std::fixed << std::setprecision(2)
             << value * 1e-9 << " GB/s (phase " << phaseNumber << ", executor " << info.originalRanks[i] << ")\n";
    }
    if (worldRank == 0) output->recordPhaseDiagnostics(text.str());
}

PairPhases planConcurrentPairPhases(const std::vector<std::pair<int, int>> &pairs, bool unorderedPairs) {
    std::set<std::pair<std::string, std::string>> ppusInUse;
    for (size_t rank = 0; rank < ppuHostByRank.size() && rank < ppuBDFByRank.size(); rank++) {
        if (!ppusInUse.insert({ppuHostByRank[rank], ppuBDFByRank[rank]}).second) {
            throw std::string("--maxConcurrentPairs >= 0 requires every MPI rank to use a different PPU");
        }
    }

    PairPhases phases = planPairPhases(pairs, worldSize, unorderedPairs,
                                       static_cast<size_t>(gSettings.maxConcurrentPairs));
    unsigned long long digest = pairPhasesDigest(phases);
    unsigned long long minDigest = 0;
    unsigned long long maxDigest = 0;
    ASSERT(MPI_Allreduce(&digest, &minDigest, 1, MPI_UNSIGNED_LONG_LONG, MPI_MIN, MPI_COMM_WORLD) == MPI_SUCCESS);
    ASSERT(MPI_Allreduce(&digest, &maxDigest, 1, MPI_UNSIGNED_LONG_LONG, MPI_MAX, MPI_COMM_WORLD) == MPI_SUCCESS);
    if (minDigest != maxDigest) {
        throw std::string("The concurrent pair schedule differs between MPI ranks");
    }

    if (gSettings.verbose && worldRank == 0) {
        size_t pairCount = 0;
        size_t largestPhase = 0;
        for (const auto &phase : phases) {
            pairCount += phase.size();
            largestPhase = std::max(largestPhase, phase.size());
        }
        std::ostringstream summary;
        summary << "Concurrent pair schedule: " << pairCount << (unorderedPairs ? " unordered" : " ordered")
                << " rank pairs in " << phases.size() << " phases, up to " << largestPhase << " pairs per phase\n";
        output->recordPhaseDiagnostics(summary.str());
    }
    return phases;
}

#endif  // MULTINODE
