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

#ifndef MULTINODE_MEMCPY_H_
#define MULTINODE_MEMCPY_H_
#ifdef MULTINODE

#include "ppu_common.h"
#include "memcpy.h"

class MultinodeMemoryAllocation {
 protected:
    void* buffer = nullptr;
    size_t bufferSize;
    int MPI_rank;

 public:
    MultinodeMemoryAllocation(size_t bufferSize, int MPI_rank);
    void *getBuffer() { return (void *) buffer; }
    HGresult streamSynchronizeWrapper(HGstream stream) const;
};

// Class responsible for allocating memory that is shareable on an ICNLink system, following RAII principles
// Constructor takes as parameters:
// - bufferSize: size of requested allocation
// - MPI_rank: node on which the allocation physically resides.
//      All other nodes will have this allocation mapped and accessible remotely.
class MultinodeMemoryAllocationUnicast : public MultinodeMemoryAllocation {
 private:
    HGmemGenericAllocationHandle handle = {};
    HGmemFabricHandle fh = {};
    HGmemAllocationHandleType handleType = {};
    HGmemAllocationProp prop = {};
    HGmemAccessDesc desc = {};
    size_t roundedUpAllocationSize;

 public:
    MultinodeMemoryAllocationUnicast(size_t bufferSize, int MPI_rank);
    ~MultinodeMemoryAllocationUnicast();
};

// Class responsible for allocating multicast object, following RAII principles
// Constructor takes as parameters:
// - bufferSize: size of requested allocation
// - MPI_rank: node driving the allocation process and exporting memory handle.
//      All nodes will have this allocation mapped and accessible.
class MultinodeMemoryAllocationMulticast : public MultinodeMemoryAllocation {
 private:
    HGmemGenericAllocationHandle handle = {};
    HGmemGenericAllocationHandle multicastHandle = {};
    HGmemFabricHandle fh = {};
    HGmemAllocationHandleType handleType = {};
    HGmulticastObjectProp multicastProp = {};
    HGmemAccessDesc desc = {};
    size_t roundedUpAllocationSize;
 public:
    MultinodeMemoryAllocationMulticast(size_t bufferSize, int MPI_rank);
    ~MultinodeMemoryAllocationMulticast();
};

// Class responsible for implementing Multinode MemcpyBuffer
// Each instance has information about which node owns the memory
class MultinodeDeviceBuffer : public MemcpyBuffer {
 private:
    int MPI_rank;
    // Retained once here rather than on every getPrimaryCtx() call, which never released it.
    HGcontext primaryCtx {};
 public:
    MultinodeDeviceBuffer(size_t bufferSize, int MPI_rank);
    ~MultinodeDeviceBuffer() override;

    virtual HGcontext getPrimaryCtx() const override;
    virtual int getBufferIdx() const override;
    virtual std::string getBufferString() const override;
    virtual int getMPIRank() const override;
    bool isDeviceBuffer() const override;
};

// MemcpyBuffer containing memory accessible from a different node in a multi-node ICNLink connected system
// MPI_rank node owns the memory allocation, other nodes have it mapped
// Writes/reads to that memory from other nodes happen over ICNLink
class MultinodeDeviceBufferUnicast : public MultinodeDeviceBuffer {
 private:
    MultinodeMemoryAllocationUnicast MemoryAllocation;
 public:
    MultinodeDeviceBufferUnicast(size_t bufferSize, int MPI_rank);
};

// MemcpyBuffer containing memory bound to multicast object
// Each node has its own copy of the memory, and the copies are the same
// Writes to this memory are instantly propagated to other nodes (conforming to P2P writes memory model)
class MultinodeDeviceBufferMulticast : public MultinodeDeviceBuffer {
 private:
    MultinodeMemoryAllocationMulticast MemoryAllocation;
 public:
    MultinodeDeviceBufferMulticast(size_t bufferSize, int MPI_rank);
};

// MemcpyBuffer containing regular device memory
// Only available on one node, exists primarily to simplify writing testcases
class MultinodeDeviceBufferLocal : public MultinodeDeviceBuffer {
 private:
    HGcontext primaryCtx {};
 public:
    MultinodeDeviceBufferLocal(size_t bufferSize, int MPI_rank);
    ~MultinodeDeviceBufferLocal();
};

class NodeHelperMulti : public NodeHelper {
 private:
    int rankOfFirstMemcpy;

    // streamBlocker
    volatile int* blockingVarHost;
    volatile int* blockingVarDevice;
    MultinodeMemoryAllocationUnicast blockingVarDeviceAllocation;
 public:
    NodeHelperMulti();
    ~NodeHelperMulti();
    MemcpyDispatchInfo dispatchMemcpy(const std::vector<const MemcpyBuffer*> &srcBuffers, const std::vector<const MemcpyBuffer*> &dstBuffers, P2PType p2pType, ContextPreference ctxPreference);
    double calculateTotalBandwidth(double totalTime, double totalSize, size_t loopCount);
    double calculateSumBandwidth(std::vector<PerformanceStatistic> &bandwidthStats);
    double calculateFirstBandwidth(std::vector<PerformanceStatistic> &bandwidthStats);
    std::vector<double> calculateVectorBandwidth(std::vector<double> &results, std::vector<int> originalRanks);
    void synchronizeProcess();
    HGresult streamSynchronizeWrapper(HGstream stream) const;

    // stream blocking functions
    void streamBlockerReset();
    void streamBlockerRelease();
    void streamBlockerBlock(HGstream mainstream, HGstream die0stream, HGstream die1stream);
};

#endif  // MULTINODE
#endif  // MULTINODE_MEMCPY_H_
