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

#ifndef MEMCPY_H_
#define MEMCPY_H_

#include <memory>
#include "inline_common.h"
#include "ppu_common.h"
#include "common.h"

class MemcpyBuffer {
 protected:
    void* buffer{};
    size_t bufferSize;
 public:
    MemcpyBuffer(size_t bufferSize);
    virtual ~MemcpyBuffer() {}
    virtual HGdeviceptr getGidBuffer() const { return 0; }
    HGdeviceptr getBuffer() const;
    size_t getBufferSize() const;

    virtual int getBufferIdx() const = 0;
    virtual bool isDeviceBuffer() const = 0;
    virtual HGcontext getPrimaryCtx() const = 0;
    virtual std::string getBufferString() const = 0;
    // In MPI configuration we want to avoid using blocking functions such as hgStreamSynchronize to adhere to MPI notion of progress
    virtual HGresult streamSynchronizeWrapper(HGstream stream) const;
    virtual int getMPIRank() const;
};

// Represents the host buffer abstraction
class HostBuffer : public MemcpyBuffer {
 public:
    // NUMA affinity is set here through allocation of memory in the socket group where `targetDeviceId` resides
    HostBuffer(size_t bufferSize, int targetDeviceId);
    ~HostBuffer();

    int getBufferIdx() const override;
    bool isDeviceBuffer() const override;
    HGcontext getPrimaryCtx() const override;
    virtual std::string getBufferString() const override;
};

// Represents the device buffer and context abstraction
class DeviceBuffer : public MemcpyBuffer {
 private:
    int deviceIdx;
    HGcontext primaryCtx{};
 public:
    DeviceBuffer(size_t bufferSize, int deviceIdx);
    ~DeviceBuffer();

    int getBufferIdx() const override;
    bool isDeviceBuffer() const override;
    HGcontext getPrimaryCtx() const override;
    virtual std::string getBufferString() const override;

    bool enablePeerAccess(const DeviceBuffer &peerBuffer);
};

// Represents the device virtual buffer and context abstraction
class DeviceBufferVm : public MemcpyBuffer {
 private:
    void* gidBuffer{};
    int deviceIdx;
    HGcontext primaryCtx{};
    HGmemGenericAllocationHandle handle;
    HGmemGenericAllocationHandle gidHandle;

   //  DeviceVmAllocationKind kind{DeviceVmAllocationKind::Data};
 public:
    DeviceBufferVm(size_t bufferSize, int deviceIdx);
   //  DeviceBufferVm(size_t bufferSize, int deviceIdx, DeviceVmAllocationKind kind);
    ~DeviceBufferVm();
   
    HGdeviceptr getGidBuffer() const override;
    int getBufferIdx() const override;
    bool isDeviceBuffer() const override;
    HGcontext getPrimaryCtx() const override;
    virtual std::string getBufferString() const override;

    bool enableReadOnlyAccessFromPeer(const DeviceBufferVm &peerBuffer);
};

// Describe attributes of a single memcpy operation
class MemcpyDescriptor {
 public:
    const MemcpyBuffer* dstBuffer;
    const MemcpyBuffer* srcBuffer;
    HGstream mainstream;
    HGstream die0stream;
    HGstream die1stream;
    size_t copySize;
    unsigned long long loopCount;
    
    // icn read
    IcnReadConfig icnReadConfig;

    MemcpyDescriptor(const MemcpyBuffer* dstBuffer,
                     const MemcpyBuffer* srcBuffer,
                     HGstream mainstream,
                     HGstream die0stream,
                     HGstream die1stream,
                     size_t copySize,
                     unsigned long long loopCount,
                     const IcnReadConfig& icnReadConfig);
};

// Specifies the preferred node's context to do the operation from
// It's only a preference because if the preferred node is a HostBuffer, it has no context and will fall back to the other node
enum ContextPreference {
        PREFER_SRC_CONTEXT,    // Prefer the source buffer's context if available
        PREFER_DST_CONTEXT     // Prefer the destination buffer's context if available
};

class MemcpyOperation;

// forward declaration
class NodeHelper;
class MemcpyInitiator;

enum P2PType {
        CE,
        SM,
        MULTI_CAST,
        SM_SPLIT,
};

class MemcpyDispatchInfo {
 public:
    P2PType p2pType;
    bool isSingleDie;
    
    // use vpipe id and enable read optimize
    IcnReadConfig icnReadConfig;

    std::vector<HGcontext> contexts;
    std::vector<HGstream> mainstreams;
    std::vector<HGevent> mainStartFlags;
    std::vector<HGevent> mainStopFlags;

    std::vector<HGstream> die0streams;
    std::vector<HGstream> die1streams;
    std::vector<HGevent> die0StartFlags;
    std::vector<HGevent> die0StopFlags;
    std::vector<HGevent> die1StartFlags;
    std::vector<HGevent> die1StopFlags;

    std::vector<const MemcpyBuffer*> srcBuffers;
    std::vector<const MemcpyBuffer*> dstBuffers;
    std::vector<int> originalRanks;
    std::vector<size_t> adjustedCopySizes;
    std::shared_ptr<NodeHelper> nodeHelper;
    MemcpyDispatchInfo(P2PType p2pType, bool isSingleDie, std::vector<HGcontext> contexts, std::vector<const MemcpyBuffer*> srcBuffers,
                       std::vector<const MemcpyBuffer*> dstBuffers, std::vector<int> originalRanks = {}, const IcnReadConfig& icnReadConfig = {});

    void initInfo(const std::shared_ptr<MemcpyInitiator>& memcpyInitiator);
    void freeInfo(const std::shared_ptr<MemcpyInitiator>& memcpyInitiator);
    void sync(int buffIdx);
    void recordStart(int buffIdx);
    void recordStop(int buffIdx);
    void waitStart(int buffIdx, int waitIdx);
    void waitStop(int buffIdx, int waitIdx);
    void elapsedTime(int buffIdx, double *bandwidth, unsigned long long loopCount);
};

class NodeHelper {
 public:
    virtual MemcpyDispatchInfo dispatchMemcpy(const std::vector<const MemcpyBuffer*> &srcBuffers, const std::vector<const MemcpyBuffer*> &dstBuffers, P2PType p2pType, ContextPreference ctxPreference) = 0;
    virtual double calculateTotalBandwidth(double totalTime, double totalSize, size_t loopCount) = 0;
    virtual double calculateSumBandwidth(std::vector<PerformanceStatistic> &bandwidthStats) = 0;
    virtual double calculateFirstBandwidth(std::vector<PerformanceStatistic> &bandwidthStats) = 0;
    virtual std::vector<double> calculateVectorBandwidth(std::vector<double> &results, std::vector<int> originalRanks) = 0;
    virtual void synchronizeProcess() = 0;
    // In MPI configuration we want to avoid using blocking functions such as hgStreamSynchronize to adhere to MPI notion of progress
    virtual HGresult streamSynchronizeWrapper(HGstream stream) const = 0;

    // stream blocking functions
    virtual void streamBlockerReset() = 0;
    virtual void streamBlockerRelease() = 0;
    virtual void streamBlockerBlock(HGstream mainstream, HGstream die0stream, HGstream die1stream) = 0;

    virtual ~NodeHelper() = default;
};

class NodeHelperSingle : public NodeHelper {
 private:
    volatile int* blockingVarHost;
 public:
    NodeHelperSingle();
    ~NodeHelperSingle();
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

class MemcpyInitiator {
 public:
    const P2PType p2pType;
    explicit MemcpyInitiator(P2PType type) : p2pType(type) {}
    P2PType getP2PType() const noexcept { return p2pType; }

    // Pure virtual function for implementation of the actual memcpy function
    // return actual bytes copied
    // This can vary from copySize due to SM copies truncated the copy to achieve max bandwidth
    virtual size_t memcpyFunc(MemcpyDescriptor &memcpyDescriptor) = 0;
    // Calculate the truncated sizes used by copy kernels
    virtual size_t getAdjustedCopySize(size_t size, HGstream stream, bool use_bulk) = 0;
    // Fill buffer with a pattern
    virtual void memsetPattern(MemcpyDispatchInfo &info) const = 0;
    // Compare buffer with a pattern
    virtual void memcmpPattern(MemcpyDispatchInfo &info) const = 0;
    // Adjust the bandwidth before final reporting
    virtual double getAdjustedBandwidth(double bandwidth) = 0;

    virtual ~MemcpyInitiator() = default;
};

class MemcpyInitiatorSM : public MemcpyInitiator {
 protected:
    explicit MemcpyInitiatorSM(P2PType type) : MemcpyInitiator(type) {}

 public:
    MemcpyInitiatorSM() : MemcpyInitiator(P2PType::SM) {}

    size_t memcpyFunc(MemcpyDescriptor &memcpyDescriptor);
    // Calculate the truncated sizes used by copy kernels
    size_t getAdjustedCopySize(size_t size, HGstream stream, bool use_bulk);
    // Fill buffer with a pattern
    void memsetPattern(MemcpyDispatchInfo &info) const;
    // Compare buffer with a pattern
    void memcmpPattern(MemcpyDispatchInfo &info) const;
    // Adjust the bandwidth before final reporting
    double getAdjustedBandwidth(double bandwidth);
};

class MemcpyInitiatorCE : public MemcpyInitiator  {
 public:
    MemcpyInitiatorCE() : MemcpyInitiator(P2PType::CE) {}

    size_t memcpyFunc(MemcpyDescriptor &memcpyDescriptor);
    // Calculate the truncated sizes used by copy kernels
    size_t getAdjustedCopySize(size_t size, HGstream stream, bool use_bulk);
    // Fill buffer with a pattern
    void memsetPattern(MemcpyDispatchInfo &info) const;
    // Compare buffer with a pattern
    void memcmpPattern(MemcpyDispatchInfo &info) const;
    // Adjust the bandwidth before final reporting
    double getAdjustedBandwidth(double bandwidth);
};

class MemcpyInitiatorMulticastWrite : public MemcpyInitiator {
 public:
    MemcpyInitiatorMulticastWrite() : MemcpyInitiator(P2PType::MULTI_CAST) {}

    size_t memcpyFunc(MemcpyDescriptor &memcpyDescriptor);
    // Calculate the truncated sizes used by copy kernels
    size_t getAdjustedCopySize(size_t size, HGstream stream, bool use_bulk);
    // Fill buffer with a pattern
    void memsetPattern(MemcpyDispatchInfo &info) const;
    // Compare buffer with a pattern
    void memcmpPattern(MemcpyDispatchInfo &info) const;
    // Adjust the bandwidth before final reporting
    double getAdjustedBandwidth(double bandwidth);
};

class MemcpyInitiatorSMSplitWarp : public MemcpyInitiatorSM {
 public:
    MemcpyInitiatorSMSplitWarp() : MemcpyInitiatorSM(SM_SPLIT) {}

    size_t memcpyFunc(MemcpyDescriptor &memcpyDescriptor);
    // Calculate the truncated sizes used by copy kernels
    size_t getAdjustedCopySize(size_t size, HGstream stream, bool use_bulk);
    // Fill buffer with a pattern
    void memsetPattern(MemcpyDispatchInfo &info) const;
    // Compare buffer with a pattern
    void memcmpPattern(MemcpyDispatchInfo &info) const;
    // Adjust the bandwidth before final reporting
    double getAdjustedBandwidth(double bandwidth);
};

// Abstraction of a memory Operation.
class MemoryOperation {
 public:
    MemoryOperation() = default;
    ~MemoryOperation() = default;
};

// Abstraction of a memcpy operation
class MemcpyOperation : public MemoryOperation {
 public:
    // Specifies which bandwidths to use for the final result of simultaneous copies
    enum BandwidthValue {
            USE_FIRST_BW,      // Use the bandwidth of the first copy in the simultaneous copy list
            SUM_BW,            // Use the sum of all bandwidths from the simultaneous copy list
            TOTAL_BW,          // Use the total bandwidth of all copies, based on total time and total bytes copied
            VECTOR_BW,         // Return bandwidths of each copy separately
    };

 private:
    unsigned long long loopCount;

 protected:
    size_t *procMask;

    std::shared_ptr<MemcpyInitiator> memcpyInitiator;
    std::shared_ptr<NodeHelper> nodeHelper;
    ContextPreference ctxPreference;
    BandwidthValue bandwidthValue;

 public:
    MemcpyOperation(unsigned long long loopCount, MemcpyInitiator *_memcpyInitiator, ContextPreference ctxPreference = ContextPreference::PREFER_SRC_CONTEXT, BandwidthValue bandwidthValue = BandwidthValue::USE_FIRST_BW);
    MemcpyOperation(unsigned long long loopCount, MemcpyInitiator *_memcpyInitiator, NodeHelper *_nodeHelper, ContextPreference ctxPreference = ContextPreference::PREFER_SRC_CONTEXT, BandwidthValue bandwidthValue = BandwidthValue::USE_FIRST_BW);
    virtual ~MemcpyOperation();

    IcnReadConfig icnReadConfig = {};
    // Lists of paired nodes will be executed simultaneously
    // context of srcBuffers is preferred (if not host) unless otherwise specified
    P2PType getP2PType() const { return memcpyInitiator->getP2PType(); }
    std::vector<double> doMemcpyCore(MemcpyDispatchInfo &info);
    std::vector<double> doMemcpyVector(const std::vector<const MemcpyBuffer*> &srcBuffers, const std::vector<const MemcpyBuffer*> &dstBuffers);
    double doMemcpy(const std::vector<const MemcpyBuffer*> &srcBuffers, const std::vector<const MemcpyBuffer*> &dstBuffers);
    double doMemcpy(const MemcpyBuffer &srcBuffer, const MemcpyBuffer &dstBuffer);
};

class MemPtrChaseOperation : public MemoryOperation {
 public:
    MemPtrChaseOperation(unsigned long long loopCount);
    ~MemPtrChaseOperation() = default;
    double doPtrChase(const int srcId, const MemcpyBuffer &peerBuffer);
 private:
    unsigned long long loopCount;
    unsigned int smCount;
};

#endif  // MEMCPY_H_
