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

#include "memcpy.h"
#include "output.h"
#include "verify_utils.h"
#include "kernels.cuh"
// #include "vector_types.h"
#ifdef MULTINODE
#include <mpi.h>
#include "multinode_memcpy.h"
#endif
#define WARMUP_COUNT 4
#include <cassert>

MemcpyBuffer::MemcpyBuffer(size_t bufferSize): buffer(nullptr), bufferSize(bufferSize) {}

HGdeviceptr MemcpyBuffer::getBuffer() const {
    return (HGdeviceptr)buffer;
}

size_t MemcpyBuffer::getBufferSize() const {
    return bufferSize;
}

void xorshift2MBPattern(unsigned int* buffer, unsigned int seed) {
    unsigned int oldValue = seed;
    unsigned int n = 0;
    for (n = 0; n < _2MiB / sizeof(unsigned int); n++) {
        unsigned int value = oldValue;
        value = value ^ (value << 13);
        value = value ^ (value >> 17);
        value = value ^ (value << 5);
        oldValue = value;
        buffer[n] = oldValue;
    }
}

void memsetPatternHelper(HGdeviceptr buffer, unsigned long long size, unsigned int seed, std::shared_ptr<NodeHelper> nodeHelper) {
    unsigned int* h_pattern;
    HGdeviceptr d_pattern;

    unsigned long long num_elements = size / sizeof(unsigned int);
    unsigned long long num_pattern_elements = _2MiB / sizeof(unsigned int);

    // Allocate 2MB of pattern
    HG_ASSERT(hgMemHostAlloc((void**)&h_pattern, sizeof(char) * _2MiB, HG_MEMHOSTALLOC_PORTABLE));
    xorshift2MBPattern(h_pattern, seed);

    // Copy the pattern to a device buffer
    HG_ASSERT(hgMemAlloc(&d_pattern, sizeof(char) * _2MiB));
    HG_ASSERT(hgMemcpyAsync(d_pattern, (HGdeviceptr)h_pattern, sizeof(char) * _2MiB, HG_STREAM_PER_THREAD));

    // Launch the memset kernel
    HG_ASSERT(memsetKernel(HG_STREAM_PER_THREAD, buffer, d_pattern, num_elements, num_pattern_elements));
    HG_ASSERT(nodeHelper->streamSynchronizeWrapper(HG_STREAM_PER_THREAD));

    HG_ASSERT(hgMemFreeHost((void*)h_pattern));
    HG_ASSERT(hgMemFree(d_pattern));
}

void memclearByWarpParity(HGdeviceptr buffer, unsigned long long size, bool clearOddWarpIndexed, std::shared_ptr<NodeHelper> nodeHelper) {
    HG_ASSERT(memclearKernelByWarpParity(HG_STREAM_PER_THREAD, buffer, size, clearOddWarpIndexed));
    HG_ASSERT(nodeHelper->streamSynchronizeWrapper(HG_STREAM_PER_THREAD));
}

void MemcpyInitiatorCE::memsetPattern(MemcpyDispatchInfo &info) const {
    for (int i = 0; i < info.srcBuffers.size(); i++) {
        HG_ASSERT(hgCtxSetCurrent(info.contexts[i]));
        memsetPatternHelper(info.dstBuffers[i]->getBuffer(), info.adjustedCopySizes[i], VERIFY_DST_SEED, info.nodeHelper);
        memsetPatternHelper(info.srcBuffers[i]->getBuffer(), info.adjustedCopySizes[i], VERIFY_SRC_SEED, info.nodeHelper);
    }
}

void MemcpyInitiatorSM::memsetPattern(MemcpyDispatchInfo &info) const {
    for (int i = 0; i < info.srcBuffers.size(); i++) {
        HG_ASSERT(hgCtxSetCurrent(info.contexts[i]));
        memsetPatternHelper(info.dstBuffers[i]->getBuffer(), info.adjustedCopySizes[i], VERIFY_DST_SEED, info.nodeHelper);
        memsetPatternHelper(info.srcBuffers[i]->getBuffer(), info.adjustedCopySizes[i], VERIFY_SRC_SEED, info.nodeHelper);
    }
}

void MemcpyInitiatorMulticastWrite::memsetPattern(MemcpyDispatchInfo &info) const {
    for (int i = 0; i < info.srcBuffers.size(); i++) {
        HG_ASSERT(hgCtxSetCurrent(info.contexts[i]));
        memsetPatternHelper(info.dstBuffers[i]->getBuffer(), info.adjustedCopySizes[i], VERIFY_DST_SEED, info.nodeHelper);
        memsetPatternHelper(info.srcBuffers[i]->getBuffer(), info.adjustedCopySizes[i], VERIFY_SRC_SEED, info.nodeHelper);
    }
}

void MemcpyInitiatorSMSplitWarp::memsetPattern(MemcpyDispatchInfo &info) const {
    for (int i = 0; i < info.srcBuffers.size(); i++) {
        HG_ASSERT(hgCtxSetCurrent(info.contexts[i]));
        memsetPatternHelper(info.dstBuffers[i]->getBuffer(), info.adjustedCopySizes[i], VERIFY_SRC_SEED, info.nodeHelper);
        memsetPatternHelper(info.srcBuffers[i]->getBuffer(), info.adjustedCopySizes[i], VERIFY_SRC_SEED, info.nodeHelper);
        memclearByWarpParity(info.dstBuffers[i]->getBuffer(), info.adjustedCopySizes[i], true /* clearOddWarpIndexed */, info.nodeHelper);
        memclearByWarpParity(info.srcBuffers[i]->getBuffer(), info.adjustedCopySizes[i], false /* clearOddWarpIndexed */, info.nodeHelper);
    }
}

double MemcpyInitiatorCE::getAdjustedBandwidth(double bandwidth) {
    return bandwidth;
}

double MemcpyInitiatorSM::getAdjustedBandwidth(double bandwidth) {
    return bandwidth;
}

double MemcpyInitiatorMulticastWrite::getAdjustedBandwidth(double bandwidth) {
    return bandwidth;
}

double MemcpyInitiatorSMSplitWarp::getAdjustedBandwidth(double bandwidth) {
    // For split warp copies, we estimate bandwidth in each direction as 1/2 of measured bandwidth
    return bandwidth / 2;
}

// Add this new typedef for the comparison function pointer
typedef HGresult (*CompareKernelFunc)(HGstream, HGdeviceptr, HGdeviceptr, unsigned long long, unsigned int, HGdeviceptr);

// Which endpoint buffer the check reads after the copy (the bidirectional split-warp copy also
// writes back into src).
enum VerifyTarget {
    VERIFY_DST,
    VERIFY_SRC
};

// Verify that the buffer selected by 'target' matches the transferred pattern (VERIFY_SRC_SEED).
// initSeed is that buffer's pre-fill pattern, used only for the failure report.
void memcmpPatternHelper(const MemcpyBuffer* srcBuf, const MemcpyBuffer* dstBuf, VerifyTarget target, unsigned long long size, unsigned int initSeed, CompareKernelFunc compareKernel, std::shared_ptr<NodeHelper> nodeHelper) {
    const MemcpyBuffer* verifiedBuf = (target == VERIFY_SRC) ? srcBuf : dstBuf;
    HGdeviceptr buffer = verifiedBuf->getBuffer();
    unsigned int* h_pattern;
    HGdeviceptr d_pattern;
    int h_errorFlag = 0;
    HGdeviceptr d_errorFlag;
    unsigned long long num_elements = size / sizeof(unsigned int);
    unsigned long long num_pattern_elements = _2MiB / sizeof(unsigned int);

    // Allocate 2MB of pattern
    HG_ASSERT(hgMemHostAlloc((void**)&h_pattern, sizeof(char) * _2MiB, HG_MEMHOSTALLOC_PORTABLE));
    xorshift2MBPattern(h_pattern, VERIFY_SRC_SEED);
    HG_ASSERT(hgMemAlloc(&d_pattern, sizeof(char) * _2MiB));
    HG_ASSERT(hgMemcpyAsync(d_pattern, (HGdeviceptr)h_pattern, sizeof(char) * _2MiB, HG_STREAM_PER_THREAD));

    // setup error flag
    HG_ASSERT(hgMemAlloc(&d_errorFlag, sizeof(int)));
    HG_ASSERT(hgMemcpyAsync(d_errorFlag, (HGdeviceptr)&h_errorFlag, sizeof(int), HG_STREAM_PER_THREAD));

    // Optional SAILBW_VERIFY fault injection.
    verifyInjectFaultIfRequested(buffer, size, verifyLinkId(srcBuf), verifyLinkId(dstBuf), nodeHelper);

    // launch kernel to compare
    HG_ASSERT(compareKernel(HG_STREAM_PER_THREAD, buffer, d_pattern, num_elements, num_pattern_elements, d_errorFlag));
    HG_ASSERT(nodeHelper->streamSynchronizeWrapper(HG_STREAM_PER_THREAD));
    HG_ASSERT(hgMemcpyAsync((HGdeviceptr)&h_errorFlag, d_errorFlag, sizeof(int), HG_STREAM_PER_THREAD));
    HG_ASSERT(nodeHelper->streamSynchronizeWrapper(HG_STREAM_PER_THREAD));

    HG_ASSERT(hgMemFree(d_errorFlag));
    HG_ASSERT(hgMemFree(d_pattern));

    if (h_errorFlag != 0) {
        // Regenerate the verified buffer's pre-fill pattern for the report; split-warp buffers were
        // half-cleared by warp parity, so no per-word init pattern exists for them.
        unsigned int* h_initPattern = nullptr;
        if (initSeed != VERIFY_SRC_SEED) {
            HG_ASSERT(hgMemHostAlloc((void**)&h_initPattern, sizeof(char) * _2MiB, HG_MEMHOSTALLOC_PORTABLE));
            xorshift2MBPattern(h_initPattern, initSeed);
        }
        verifyReportFailure(srcBuf, dstBuf, verifiedBuf, h_pattern, h_initPattern, num_elements, num_pattern_elements);
        if (h_initPattern != nullptr) {
            HG_ASSERT(hgMemFreeHost((void*)h_initPattern));
        }
    }
    HG_ASSERT(hgMemFreeHost((void*)h_pattern));
    ASSERT(h_errorFlag == 0);
}

void MemcpyInitiatorCE::memcmpPattern(MemcpyDispatchInfo &info) const {
    for (int i = 0; i < info.srcBuffers.size(); i++) {
        HG_ASSERT(hgCtxSetCurrent(info.contexts[i]));
        memcmpPatternHelper(info.srcBuffers[i], info.dstBuffers[i], VERIFY_DST, info.adjustedCopySizes[i], VERIFY_DST_SEED, memcmpKernel, info.nodeHelper);
    }
}

void MemcpyInitiatorSM::memcmpPattern(MemcpyDispatchInfo &info) const {
    for (int i = 0; i < info.srcBuffers.size(); i++) {
        HG_ASSERT(hgCtxSetCurrent(info.contexts[i]));
        memcmpPatternHelper(info.srcBuffers[i], info.dstBuffers[i], VERIFY_DST, info.adjustedCopySizes[i], VERIFY_DST_SEED, memcmpKernel, info.nodeHelper);
    }
}

void MemcpyInitiatorMulticastWrite::memcmpPattern(MemcpyDispatchInfo &info) const {
    for (int i = 0; i < info.srcBuffers.size(); i++) {
        HG_ASSERT(hgCtxSetCurrent(info.contexts[i]));
        memcmpPatternHelper(info.srcBuffers[i], info.dstBuffers[i], VERIFY_DST, info.adjustedCopySizes[i], VERIFY_DST_SEED, multicastMemcmpKernel, info.nodeHelper);
    }
}

void MemcpyInitiatorSMSplitWarp::memcmpPattern(MemcpyDispatchInfo &info) const {
    for (int i = 0; i < info.srcBuffers.size(); i++) {
        HG_ASSERT(hgCtxSetCurrent(info.contexts[i]));
        // src and dst buffer contents must match after the bidirectional split warp copy
        memcmpPatternHelper(info.srcBuffers[i], info.dstBuffers[i], VERIFY_DST, info.adjustedCopySizes[i], VERIFY_SRC_SEED, memcmpKernel, info.nodeHelper);
        memcmpPatternHelper(info.srcBuffers[i], info.dstBuffers[i], VERIFY_SRC, info.adjustedCopySizes[i], VERIFY_SRC_SEED, memcmpKernel, info.nodeHelper);
    }
}

int MemcpyBuffer::getMPIRank() const {
    return -1;
}

HGresult MemcpyBuffer::streamSynchronizeWrapper(HGstream stream) const {
    return hgStreamSynchronize(stream);
}

HostBuffer::HostBuffer(size_t bufferSize, int targetDeviceId): MemcpyBuffer(bufferSize) {
    HGcontext targetCtx = nullptr;

    // Before allocating host memory, set correct NUMA affinity
    setOptimalCpuAffinity(targetDeviceId);
    HG_ASSERT(hgDevicePrimaryCtxRetain(&targetCtx, targetDeviceId));
    HG_ASSERT(hgCtxSetCurrent(targetCtx));

    HG_ASSERT(hgMemHostAlloc(&buffer, bufferSize, HG_MEMHOSTALLOC_PORTABLE));
}

HostBuffer::~HostBuffer() {
    if (isMemoryOwnedByHGGC(buffer)) {
        HG_ASSERT(hgMemFreeHost(buffer));
    } else {
        free(buffer);
    }
}

// Host nodes don't have a context, return null
HGcontext HostBuffer::getPrimaryCtx() const {
    return nullptr;
}

// Host buffers always return zero as they always represent one row in the bandwidth matrix
int HostBuffer::getBufferIdx() const {
    return 0;
}

bool HostBuffer::isDeviceBuffer() const {
    return false;
}

std::string HostBuffer::getBufferString() const {
    return "Host";
}

DeviceBuffer::DeviceBuffer(size_t bufferSize, int deviceIdx): MemcpyBuffer(bufferSize), deviceIdx(deviceIdx) {
    HG_ASSERT(hgDevicePrimaryCtxRetain(&primaryCtx, deviceIdx));
    HG_ASSERT(hgCtxSetCurrent(primaryCtx));
    HG_ASSERT(hgMemAlloc((HGdeviceptr*)&buffer, bufferSize));
}

DeviceBuffer::~DeviceBuffer() {
    HG_ASSERT(hgCtxSetCurrent(primaryCtx));
    HG_ASSERT(hgMemFree((HGdeviceptr)buffer));
    HG_ASSERT(hgDevicePrimaryCtxRelease(deviceIdx));
}

HGcontext DeviceBuffer::getPrimaryCtx() const {
    return primaryCtx;
}

int DeviceBuffer::getBufferIdx() const {
    return deviceIdx;
}

bool DeviceBuffer::isDeviceBuffer() const {
    return true;
}

std::string DeviceBuffer::getBufferString() const {
    return "Device " + std::to_string(deviceIdx);
}

bool DeviceBuffer::enablePeerAccess(const DeviceBuffer &peerBuffer) {
    int canAccessPeer = 0;
    HG_ASSERT(hgDeviceCanAccessPeer(&canAccessPeer, getBufferIdx(), peerBuffer.getBufferIdx()));
    if (canAccessPeer) {
        HGresult res;
        HG_ASSERT(hgCtxSetCurrent(peerBuffer.getPrimaryCtx()));
        res = hgCtxEnablePeerAccess(getPrimaryCtx(), 0);
        if (res != HGGC_ERROR_PEER_ACCESS_ALREADY_ENABLED)
            HG_ASSERT(res);

        HG_ASSERT(hgCtxSetCurrent(getPrimaryCtx()));
        res = hgCtxEnablePeerAccess(peerBuffer.getPrimaryCtx(), 0);
        if (res != HGGC_ERROR_PEER_ACCESS_ALREADY_ENABLED)
            HG_ASSERT(res);

        return true;
    }
    return false;
}

DeviceBufferVm::DeviceBufferVm(size_t bufferSize, int deviceIdx) : MemcpyBuffer(bufferSize), deviceIdx(deviceIdx), handle(0), gidHandle(0) {
    HG_ASSERT(hgDevicePrimaryCtxRetain(&primaryCtx, deviceIdx));
    HG_ASSERT(hgCtxSetCurrent(primaryCtx));

    HGmemAllocationProp prop = {};
    prop.type = HG_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = HG_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = deviceIdx;
    size_t granularity;
    HG_ASSERT(hgMemGetAllocationGranularity(&granularity, &prop, HG_MEM_ALLOC_GRANULARITY_MINIMUM));
    // Ensure size matches granularity requirements for the allocation
    this->bufferSize = ROUND_UP(bufferSize, granularity);
    
    if (ppuVpipeIdEnable) {
        // Allocate gid memory
        HG_ASSERT(EitTableProvider::hgMemCreateEx(&gidHandle, this->bufferSize, &prop, HG_MEM_CREATE_COPY_PRIMITIVE));
        HG_ASSERT(hgMemAddressReserve((HGdeviceptr *)&gidBuffer, this->bufferSize, 0, 0, 0));
        HG_ASSERT(hgMemMap((HGdeviceptr)gidBuffer, this->bufferSize, 0, gidHandle, 0));

        HGmemAccessDesc gidAccessDesc = {};
        gidAccessDesc.location.type = HG_MEM_LOCATION_TYPE_DEVICE;
        gidAccessDesc.location.id = deviceIdx;
        gidAccessDesc.flags = HG_MEM_ACCESS_FLAGS_PROT_READWRITE;
        HG_ASSERT(hgMemSetAccess((HGdeviceptr)gidBuffer, this->bufferSize, &gidAccessDesc, 1));
    }
    // Allocate physical memory
    HG_ASSERT(hgMemCreate(&handle, this->bufferSize, &prop, 0));
    HG_ASSERT(hgMemAddressReserve((HGdeviceptr *)&buffer, this->bufferSize, 0, 0, 0));
    HG_ASSERT(hgMemMap((HGdeviceptr)buffer, this->bufferSize, 0, handle, 0));

    HGmemAccessDesc accessDesc = {};
    accessDesc.location.type = HG_MEM_LOCATION_TYPE_DEVICE;
    accessDesc.location.id = deviceIdx;
    accessDesc.flags = HG_MEM_ACCESS_FLAGS_PROT_READWRITE;
    HG_ASSERT(hgMemSetAccess((HGdeviceptr)buffer, this->bufferSize, &accessDesc, 1));
}

DeviceBufferVm::~DeviceBufferVm() {
    HG_ASSERT(hgCtxSetCurrent(primaryCtx));
    HG_ASSERT(hgMemUnmap((HGdeviceptr)buffer, bufferSize));
    HG_ASSERT(hgMemRelease(handle));
    HG_ASSERT(hgMemAddressFree((HGdeviceptr)buffer, bufferSize));

    if (ppuVpipeIdEnable) {
        HG_ASSERT(hgMemUnmap((HGdeviceptr)gidBuffer, bufferSize));
        HG_ASSERT(hgMemRelease(gidHandle));
        HG_ASSERT(hgMemAddressFree((HGdeviceptr)gidBuffer, bufferSize));
    }

    HG_ASSERT(hgDevicePrimaryCtxRelease(deviceIdx));
}

HGcontext DeviceBufferVm::getPrimaryCtx() const {
    return primaryCtx;
}

HGdeviceptr DeviceBufferVm::getGidBuffer() const {
    return (HGdeviceptr)gidBuffer;
}

int DeviceBufferVm::getBufferIdx() const {
    return deviceIdx;
}

bool DeviceBufferVm::isDeviceBuffer() const {
    return true;
}

std::string DeviceBufferVm::getBufferString() const {
    return "DeviceVm " + std::to_string(deviceIdx);
}

bool DeviceBufferVm::enableReadOnlyAccessFromPeer(const DeviceBufferVm &peerBuffer) {
    int canAccessPeer = 0;
    HG_ASSERT(hgDeviceCanAccessPeer(&canAccessPeer, getBufferIdx(), peerBuffer.getBufferIdx()));
    if (canAccessPeer) {
        HG_ASSERT(hgCtxSetCurrent(peerBuffer.getPrimaryCtx()));
        HGmemAccessDesc accessDesc = {};
        accessDesc.location.type = HG_MEM_LOCATION_TYPE_DEVICE;
        accessDesc.location.id = deviceIdx;
        accessDesc.flags = HG_MEM_ACCESS_FLAGS_PROT_READWRITE;
        HG_ASSERT(hgMemSetAccess(peerBuffer.getBuffer(), peerBuffer.getBufferSize(), &accessDesc, 1));

        return true;
    }
    return false;
}

MemcpyDescriptor::MemcpyDescriptor(const MemcpyBuffer* dstBuffer, const MemcpyBuffer* srcBuffer, HGstream mainstream, HGstream die0stream, HGstream die1stream, 
                                    size_t copySize, unsigned long long loopCount, const IcnReadConfig& icnReadConfig) :
    dstBuffer(dstBuffer), srcBuffer(srcBuffer), mainstream(mainstream), die0stream(die0stream), die1stream(die1stream), copySize(copySize), loopCount(loopCount), icnReadConfig(icnReadConfig) {}

MemcpyOperation::MemcpyOperation(unsigned long long loopCount, MemcpyInitiator* memcpyInitiator, ContextPreference ctxPreference, BandwidthValue bandwidthValue) :
    MemcpyOperation(loopCount, memcpyInitiator, new NodeHelperSingle(), ctxPreference, bandwidthValue) {}

MemcpyOperation::MemcpyOperation(unsigned long long loopCount, MemcpyInitiator* memcpyInitiator, NodeHelper* nodeHelper, ContextPreference ctxPreference, BandwidthValue bandwidthValue) :
        loopCount(loopCount), memcpyInitiator(memcpyInitiator), nodeHelper(nodeHelper), ctxPreference(ctxPreference), bandwidthValue(bandwidthValue) {
    procMask = (size_t *)calloc(1, PROC_MASK_SIZE);
    PROC_MASK_SET(procMask, getFirstEnabledCPU());
}

MemcpyOperation::~MemcpyOperation() {
    PROC_MASK_CLEAR(procMask, 0);
}

double MemcpyOperation::doMemcpy(const MemcpyBuffer &srcBuffer, const MemcpyBuffer &dstBuffer) {
    std::vector<const MemcpyBuffer*> srcBuffers = {&srcBuffer};
    std::vector<const MemcpyBuffer*> dstBuffers = {&dstBuffer};
    return doMemcpy(srcBuffers, dstBuffers);
}

MemcpyDispatchInfo::MemcpyDispatchInfo(P2PType p2pType, bool isSingleDie, std::vector<HGcontext> contexts, std::vector<const MemcpyBuffer*> srcBuffers,
                                       std::vector<const MemcpyBuffer*> dstBuffers, std::vector<int> originalRanks, const IcnReadConfig& icnReadConfig) :
    p2pType(p2pType), isSingleDie(isSingleDie), icnReadConfig(icnReadConfig), contexts(contexts), srcBuffers(srcBuffers), dstBuffers(dstBuffers), originalRanks(originalRanks) {
}

void MemcpyDispatchInfo::initInfo(const std::shared_ptr<MemcpyInitiator>& memcpyInitiator) {
    size_t buffSize = srcBuffers.size();
    if (buffSize == 0) return;

    adjustedCopySizes.resize(buffSize);
    bool use_bulk = !useNormalCopy && dstBuffers[0]->isDeviceBuffer() && srcBuffers[0]->isDeviceBuffer();
    P2PType p2pType = memcpyInitiator->getP2PType();
    if (p2pType == P2PType::CE && sm >= 89 && !icnReadConfig.useSingleLinkForIcnRead) {
        die0streams.resize(buffSize);
        die1streams.resize(buffSize);
        die0StartFlags.resize(buffSize);
        die0StopFlags.resize(buffSize);
        die1StartFlags.resize(buffSize);
        die1StopFlags.resize(buffSize);

        bool isRead = (contexts[0] == dstBuffers[0]->getPrimaryCtx());

        for (int i = 0; i < buffSize; i++) {
            HG_ASSERT(hgCtxSetCurrent(contexts[i]));
            // allocate the per simultaneous copy resources
            HG_ASSERT(hgStreamCreate(&die0streams[i], HG_STREAM_NON_BLOCKING));
            HG_ASSERT(hgStreamCreate(&die1streams[i], HG_STREAM_NON_BLOCKING));
            HG_ASSERT(hgEventCreate(&die0StartFlags[i], HG_EVENT_DEFAULT));
            HG_ASSERT(hgEventCreate(&die0StopFlags[i], HG_EVENT_DEFAULT));
            HG_ASSERT(hgEventCreate(&die1StartFlags[i], HG_EVENT_DEFAULT));
            HG_ASSERT(hgEventCreate(&die1StopFlags[i], HG_EVENT_DEFAULT));
            
            // bind streams to special vpipe id
            if (icnReadConfig.enableVpipeIdReadOptimize && isRead && !disableVm) {
                int linkIdx = i % PPU_ICN_LINK_PER_DIE();
                HG_ASSERT(EitTableProvider::setStreamToVpipeId(die0streams[i], dstBuffers[i]->getGidBuffer(), linkIdx));
                HG_ASSERT(EitTableProvider::setStreamToVpipeId(die1streams[i], dstBuffers[i]->getGidBuffer(), linkIdx));
            }

            // Get the final copy size that will be used.
            // CE and SM copy sizes will differ due to possible truncation
            // during SM copies.
            adjustedCopySizes[i] = memcpyInitiator->getAdjustedCopySize(srcBuffers[i]->getBufferSize(), die0streams[i], use_bulk);
        }
    } else {
        mainstreams.resize(buffSize);
        mainStartFlags.resize(buffSize);
        mainStopFlags.resize(buffSize);

        for (int i = 0; i < buffSize; i++) {
            HG_ASSERT(hgCtxSetCurrent(contexts[i]));
            // allocate the per simultaneous copy resources
            HG_ASSERT(hgStreamCreate(&mainstreams[i], HG_STREAM_NON_BLOCKING));
            HG_ASSERT(hgEventCreate(&mainStartFlags[i], HG_EVENT_DEFAULT));
            HG_ASSERT(hgEventCreate(&mainStopFlags[i], HG_EVENT_DEFAULT));

            // single stream binds to special vpipe id
            if(icnReadConfig.useSingleLinkForIcnRead && sm >= 89 && !disableVm) {
                int linkIdx = GET_ICN_LINK_NUMBER();
                // bind to special ring buffer
                if (linkIdx <= PPU_ICN_LINK_PER_DIE()-1) {
                    HG_ASSERT(EitTableProvider::setStreamToRingId(mainstreams[i], PPU_DIE0_DMA_RING_ID_BASE)); // DIE0 ring
                } else if (linkIdx <= PPU_ICN_LINK()-1) {
                    HG_ASSERT(EitTableProvider::setStreamToRingId(mainstreams[i], PPU_DIE1_DMA_RING_ID_BASE)); // DIE1 ring
                } else {
                    std::printf("Invalid link id: %u, valid range is 0-%u \n", linkIdx, PPU_ICN_LINK() - 1);
                    ASSERT(false);
                }
                // bind to link number
                HG_ASSERT(EitTableProvider::setStreamToVpipeId(mainstreams[i], dstBuffers[i]->getGidBuffer(), static_cast<uint32_t>(linkIdx)));
            }
            // Get the final copy size that will be used.
            // CE and SM copy sizes will differ due to possible truncation
            // during SM copies.
            adjustedCopySizes[i] = memcpyInitiator->getAdjustedCopySize(srcBuffers[i]->getBufferSize(), mainstreams[i], use_bulk);
        }
    }
}

void MemcpyDispatchInfo::freeInfo(const std::shared_ptr<MemcpyInitiator>& memcpyInitiator) {
    P2PType p2pType = memcpyInitiator->getP2PType();

    if (p2pType == P2PType::CE && sm >= 89 && !icnReadConfig.useSingleLinkForIcnRead) {
        for (int i = 0; i < srcBuffers.size(); i++) {
            HG_ASSERT(hgStreamDestroy(die0streams[i]));
            HG_ASSERT(hgStreamDestroy(die1streams[i]));
            HG_ASSERT(hgEventDestroy(die0StartFlags[i]));
            HG_ASSERT(hgEventDestroy(die0StopFlags[i]));
            HG_ASSERT(hgEventDestroy(die1StartFlags[i]));
            HG_ASSERT(hgEventDestroy(die1StopFlags[i]));
        }
    } else {
        for (int i = 0; i < srcBuffers.size(); i++) {
            HG_ASSERT(hgStreamDestroy(mainstreams[i]));
            HG_ASSERT(hgEventDestroy(mainStartFlags[i]));
            HG_ASSERT(hgEventDestroy(mainStopFlags[i]));
        }
    }
}

void MemcpyDispatchInfo::sync(int buffIdx) {
    if (p2pType == P2PType::CE && sm >= 89 && !icnReadConfig.useSingleLinkForIcnRead) {
        HG_ASSERT(hgStreamSynchronize(die0streams[buffIdx]));
        if (!isSingleDie) {
            HG_ASSERT(hgStreamSynchronize(die1streams[buffIdx]));
        }
    } else {
        HG_ASSERT(hgStreamSynchronize(mainstreams[buffIdx]));
    }
}

void MemcpyDispatchInfo::recordStart(int buffIdx) {
    if (p2pType == P2PType::CE  && sm >= 89 && !icnReadConfig.useSingleLinkForIcnRead) {
        HG_ASSERT(hgEventRecord(die0StartFlags[buffIdx], die0streams[buffIdx]));
        if (!isSingleDie) {
            HG_ASSERT(hgEventRecord(die1StartFlags[buffIdx], die1streams[buffIdx]));
        }
    } else {
        HG_ASSERT(hgEventRecord(mainStartFlags[buffIdx], mainstreams[buffIdx]));
    }
}

void MemcpyDispatchInfo::recordStop(int buffIdx) {
    if (p2pType == P2PType::CE && sm >= 89 && !icnReadConfig.useSingleLinkForIcnRead) {
        HG_ASSERT(hgEventRecord(die0StopFlags[buffIdx], die0streams[buffIdx]));
        if (!isSingleDie) {
            HG_ASSERT(hgEventRecord(die1StopFlags[buffIdx], die1streams[buffIdx]));
        }
    } else {
        HG_ASSERT(hgEventRecord(mainStopFlags[buffIdx], mainstreams[buffIdx]));
    }
}

void MemcpyDispatchInfo::waitStart(int buffIdx, int waitIdx) {
    if (p2pType == P2PType::CE && sm >= 89 && !icnReadConfig.useSingleLinkForIcnRead) {
        HG_ASSERT(hgStreamWaitEvent(die0streams[buffIdx], die0StartFlags[waitIdx], 0));
        if (!isSingleDie) {
            HG_ASSERT(hgStreamWaitEvent(die1streams[buffIdx], die1StartFlags[waitIdx], 0));
        }
    } else {
        HG_ASSERT(hgStreamWaitEvent(mainstreams[buffIdx], mainStartFlags[waitIdx], 0));
    }
}

void MemcpyDispatchInfo::waitStop(int buffIdx, int waitIdx) {
    if (p2pType == P2PType::CE && sm >= 89 && !icnReadConfig.useSingleLinkForIcnRead) {
        HG_ASSERT(hgStreamWaitEvent(die0streams[buffIdx], die0StopFlags[waitIdx], 0));
        if (!isSingleDie) {
            HG_ASSERT(hgStreamWaitEvent(die1streams[buffIdx], die1StopFlags[waitIdx], 0));
        }
    } else {
        HG_ASSERT(hgStreamWaitEvent(mainstreams[buffIdx], mainStopFlags[waitIdx], 0));
    }
}

void MemcpyDispatchInfo::elapsedTime(int buffIdx, double *bandwidth, unsigned long long loopCount) {
    float mainTimeMs = 0.0f;
    if (p2pType == P2PType::CE && sm >= 89 && !icnReadConfig.useSingleLinkForIcnRead) {
        float die0TimeMs = 0.0f, die1TimeMs = 0.0f;
        HG_ASSERT(hgEventElapsedTime(&die0TimeMs, die0StartFlags[buffIdx], die0StopFlags[buffIdx]));
        if (!isSingleDie) {
            HG_ASSERT(hgEventElapsedTime(&die1TimeMs, die1StartFlags[buffIdx], die1StopFlags[buffIdx]));
        }
        mainTimeMs = die0TimeMs > die1TimeMs ? die0TimeMs : die1TimeMs;
    } else {
        HG_ASSERT(hgEventElapsedTime(&mainTimeMs, mainStartFlags[buffIdx], mainStopFlags[buffIdx]));
    }
    ASSERT(mainTimeMs > 0.0f);
    *bandwidth = (adjustedCopySizes[buffIdx] * loopCount * 1000ull) / mainTimeMs;
}

NodeHelperSingle::NodeHelperSingle() {
    HG_ASSERT(hgMemHostAlloc((void **)&blockingVarHost, sizeof(*blockingVarHost), HG_MEMHOSTALLOC_PORTABLE));
}

NodeHelperSingle::~NodeHelperSingle() {
    HG_ASSERT(hgMemFreeHost((void*)blockingVarHost));
}

MemcpyDispatchInfo NodeHelperSingle::dispatchMemcpy(const std::vector<const MemcpyBuffer*> &srcBuffers, const std::vector<const MemcpyBuffer*> &dstBuffers,
                                                    P2PType p2pType, ContextPreference ctxPreference) {
    std::vector<HGcontext> contexts(srcBuffers.size());

    for (int i = 0; i < srcBuffers.size(); i++) {
        // prefer source context
        if (ctxPreference == PREFER_SRC_CONTEXT && srcBuffers[i]->getPrimaryCtx() != nullptr) {
            contexts[i] = srcBuffers[i]->getPrimaryCtx();
        } else if (dstBuffers[i]->getPrimaryCtx() != nullptr) {
            contexts[i] = dstBuffers[i]->getPrimaryCtx();
        }
    }

    return MemcpyDispatchInfo(p2pType, isSingleDie, contexts, srcBuffers, dstBuffers);
}

double NodeHelperSingle::calculateTotalBandwidth(double totalTime, double totalSize, size_t loopCount) {
    return (totalSize * loopCount * 1000ull * 1000ull) / totalTime;
}

double NodeHelperSingle::calculateSumBandwidth(std::vector<PerformanceStatistic> &bandwidthStats) {
    double sum = 0.0;
    for (auto stat : bandwidthStats) {
        sum += stat.returnAppropriateMetric() * 1e-9;
    }
    return sum;
}

double NodeHelperSingle::calculateFirstBandwidth(std::vector<PerformanceStatistic> &bandwidthStats) {
    return bandwidthStats[0].returnAppropriateMetric() * 1e-9;
}

std::vector<double> NodeHelperSingle::calculateVectorBandwidth(std::vector<double> &results, std::vector<int> originalRanks) {
    return results;
}

void NodeHelperSingle::synchronizeProcess() {
    // NOOP
}

HGresult NodeHelperSingle::streamSynchronizeWrapper(HGstream stream) const {
    return hgStreamSynchronize(stream);
}

void NodeHelperSingle::streamBlockerReset() {
    *blockingVarHost = 0;
}

void NodeHelperSingle::streamBlockerRelease() {
    *blockingVarHost = 1;
}

void NodeHelperSingle::streamBlockerBlock(HGstream mainstream, HGstream die0stream, HGstream die1stream) {
    // start the spin kernel on the stream
    HG_ASSERT(spinKernel(blockingVarHost, mainstream, die0stream, die1stream));
}

double MemcpyOperation::doMemcpy(const std::vector<const MemcpyBuffer*> &srcBuffers, const std::vector<const MemcpyBuffer*> &dstBuffers) {
    MemcpyDispatchInfo dispatchInfo = nodeHelper->dispatchMemcpy(srcBuffers, dstBuffers, memcpyInitiator->getP2PType(), ctxPreference);
    dispatchInfo.icnReadConfig = icnReadConfig;
    auto result = doMemcpyCore(dispatchInfo);
    return result[0];
}

std::vector<double> MemcpyOperation::doMemcpyVector(const std::vector<const MemcpyBuffer*> &srcBuffers, const std::vector<const MemcpyBuffer*> &dstBuffers) {
    MemcpyDispatchInfo dispatchInfo = nodeHelper->dispatchMemcpy(srcBuffers, dstBuffers, memcpyInitiator->getP2PType(), ctxPreference);
    dispatchInfo.icnReadConfig = icnReadConfig;
    auto results = doMemcpyCore(dispatchInfo);

    return nodeHelper->calculateVectorBandwidth(results, dispatchInfo.originalRanks);
}

std::vector<double> MemcpyOperation::doMemcpyCore(MemcpyDispatchInfo &info) {
    std::vector<PerformanceStatistic> bandwidthStats(info.srcBuffers.size());
    bool isNdiesCeTest = memcpyInitiator->getP2PType() == P2PType::CE && sm >= 89 && !info.icnReadConfig.useSingleLinkForIcnRead;
    PerformanceStatistic totalBandwidth;
    HGevent totalStart;
    HGevent totalEnd;

    info.initInfo(memcpyInitiator);
    info.nodeHelper = nodeHelper;
    if (info.contexts.size() > 0) {
        HG_ASSERT(hgCtxSetCurrent(info.contexts[0]));
    }
    // If no memcpy operations are happening on this node, let's still record totalStart and totalEnd event to simplify code
    HG_ASSERT(hgEventCreate(&totalStart, HG_EVENT_DEFAULT));
    HG_ASSERT(hgEventCreate(&totalEnd, HG_EVENT_DEFAULT));

    // This loop is for sampling the testcase (which itself has a loop count)
    for (unsigned int n = 0; n < averageLoopCount; n++) {
        nodeHelper->streamBlockerReset();
        nodeHelper->synchronizeProcess();

        memcpyInitiator->memsetPattern(info);
        // block stream, and enqueue copy
        for (int i = 0; i < info.srcBuffers.size(); i++) {
            HG_ASSERT(hgCtxSetCurrent(info.contexts[i]));

            // warmup
            if (isNdiesCeTest) {
                nodeHelper->streamBlockerBlock(nullptr, info.die0streams[i], info.die1streams[i]);

                MemcpyDescriptor desc(info.dstBuffers[i], info.srcBuffers[i],
                                      nullptr, info.die0streams[i], info.die1streams[i],
                                      info.srcBuffers[i]->getBufferSize(), WARMUP_COUNT,
                                      info.icnReadConfig);
                memcpyInitiator->memcpyFunc(desc);
            } else {
                nodeHelper->streamBlockerBlock(info.mainstreams[i], nullptr, nullptr);

                MemcpyDescriptor desc(info.dstBuffers[i], info.srcBuffers[i],
                                      info.mainstreams[i], nullptr, nullptr,
                                      info.srcBuffers[i]->getBufferSize(), WARMUP_COUNT,
                                      info.icnReadConfig);
                memcpyInitiator->memcpyFunc(desc);
            }
        }

        if (info.srcBuffers.size() > 0) {
            HG_ASSERT(hgCtxSetCurrent(info.contexts[0]));
            if (isNdiesCeTest) {
                HG_ASSERT(hgEventRecord(totalStart, info.die0streams[0]));
            } else {
                HG_ASSERT(hgEventRecord(totalStart, info.mainstreams[0]));
            }
        }

        for (int i = 0; i < info.srcBuffers.size(); i++) {
            // ensure that all copies are launched at the same time
            HG_ASSERT(hgCtxSetCurrent(info.contexts[i]));
            if (i > 0) info.waitStart(i, 0);
            info.recordStart(i);
        }

        for (int i = 0; i < info.srcBuffers.size(); i++) {
            HG_ASSERT(hgCtxSetCurrent(info.contexts[i]));
            ASSERT(info.srcBuffers[i]->getBufferSize() == info.dstBuffers[i]->getBufferSize());

            if (isNdiesCeTest) {
                MemcpyDescriptor desc(info.dstBuffers[i], info.srcBuffers[i],
                                      nullptr, info.die0streams[i], info.die1streams[i],
                                      info.srcBuffers[i]->getBufferSize(), loopCount,
                                      info.icnReadConfig);
                size_t realCopySizes = memcpyInitiator->memcpyFunc(desc);
                ASSERT(info.adjustedCopySizes[i] == realCopySizes);
            } else {
                MemcpyDescriptor desc(info.dstBuffers[i], info.srcBuffers[i],
                                      info.mainstreams[i], nullptr, nullptr,
                                      info.srcBuffers[i]->getBufferSize(), loopCount,
                                      info.icnReadConfig);
                size_t realCopySizes = memcpyInitiator->memcpyFunc(desc);
                ASSERT(info.adjustedCopySizes[i] == realCopySizes);
            }

            info.recordStop(i);
            if (bandwidthValue == BandwidthValue::TOTAL_BW && i != 0) {
                // make stream0 wait on the all the others so we can measure total completion time
                info.waitStop(0, i);
            }
        }

        // record the total end - only valid if BandwidthValue::TOTAL_BW is used due to StreamWaitEvent above
        if (info.srcBuffers.size() > 0) {
            HG_ASSERT(hgCtxSetCurrent(info.contexts[0]));
            if (isNdiesCeTest) {
                HG_ASSERT(hgEventRecord(totalEnd, info.die0streams[0]));
            } else {
                HG_ASSERT(hgEventRecord(totalEnd, info.mainstreams[0]));
            }
        }

        // unblock the streams
        nodeHelper->streamBlockerRelease();
        for (int i = 0; i < info.srcBuffers.size(); i++) {
            info.sync(i);
        }
        nodeHelper->synchronizeProcess();

        if (!skipVerification) {
            memcpyInitiator->memcmpPattern(info);
        }

        for (int i = 0; i < bandwidthStats.size(); i++) {
            double bandwidth;
            info.elapsedTime(i, &bandwidth, loopCount);

            bandwidth = memcpyInitiator->getAdjustedBandwidth(bandwidth);
            bandwidthStats[i](bandwidth);

            if (bandwidthValue == BandwidthValue::SUM_BW || bandwidthValue == BandwidthValue::TOTAL_BW || i == 0) {
                // Verbose print only the values that are used for the final output
                VERBOSE << "\tSample " << n << ": " << info.srcBuffers[i]->getBufferString() << " -> " << info.dstBuffers[i]->getBufferString() << ": " <<
                    std::fixed << std::setprecision(2) << (double)bandwidth * 1e-9 << " GB/s\n";
            }
        }

        if (bandwidthValue == BandwidthValue::TOTAL_BW) {
            float totalTime = 0.0f;
            if (info.srcBuffers.size() > 0) {
                HG_ASSERT(hgEventElapsedTime(&totalTime, totalStart, totalEnd));
            }
            double elapsedTotalInUs = ((double) totalTime * 1000.0);

            // get total bytes copied
            double totalSize = 0;
            for (double size : info.adjustedCopySizes) {
                totalSize += size;
            }

            double bandwidth = nodeHelper->calculateTotalBandwidth(elapsedTotalInUs, totalSize, loopCount);
            totalBandwidth(bandwidth);
            VERBOSE << "\tSample " << n << ": Total Bandwidth : " <<
                std::fixed << std::setprecision(2) << (double)bandwidth * 1e-9 << " GB/s\n";
        }
    }

    // cleanup
    HG_ASSERT(hgEventDestroy(totalStart));
    HG_ASSERT(hgEventDestroy(totalEnd));

    info.freeInfo(memcpyInitiator);

    if (bandwidthValue == BandwidthValue::SUM_BW) {
        return {nodeHelper->calculateSumBandwidth(bandwidthStats)};
    } else if (bandwidthValue == BandwidthValue::TOTAL_BW) {
        return {totalBandwidth.returnAppropriateMetric() * 1e-9};
    } else if (bandwidthValue == BandwidthValue::VECTOR_BW) {
        std::vector<double> ret;
        for (auto stat : bandwidthStats) {
            ret.push_back(stat.returnAppropriateMetric() * 1e-9);
        }
        return ret;
    } else {
        return {nodeHelper->calculateFirstBandwidth(bandwidthStats)};
    }
}

size_t MemcpyInitiatorSM::memcpyFunc(MemcpyDescriptor &desc) {
    return copyKernel(desc);
}

size_t MemcpyInitiatorSM::getAdjustedCopySize(size_t size, HGstream stream, bool use_bulk) {
    if (!use_bulk) {
        HGdevice dev;
        HGcontext ctx;
        HG_ASSERT(hgStreamGetCtx(stream, &ctx));
        HG_ASSERT(hgCtxGetDevice(&dev));

        int blocks = nBlocks, threads = nThreads;
        if (blocks == -1 || threads == -1) {
            HG_ASSERT(hgDeviceGetAttribute(&blocks, HG_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev));
            ADJUST_SM_COUNT_FOR_SINGLE_DIE(blocks);
            threads = numThreadPerBlock;
        }

        unsigned int totalThreadCount = blocks * threads;
        // We want to calculate the exact copy sizes that will be
        // used by the copy kernels.
        if (size < (smallBufferThreshold * _MiB)) {
            // copy size is rounded down to 16 bytes
            int numUint4 = size / sizeof(uint4);
            return numUint4 * sizeof(uint4);
        }
        // adjust size to elements (size is multiple of MB, so no truncation here)
        size_t sizeInElement = size / sizeof(uint4);
        // this truncates the copy
        sizeInElement = totalThreadCount * (sizeInElement / totalThreadCount);
        return sizeInElement * sizeof(uint4);
    } else {
        return size;
    }
}

size_t MemcpyInitiatorCE::memcpyFunc(MemcpyDescriptor &desc) {
    HGdeviceptr dst = desc.dstBuffer->getBuffer();
    HGdeviceptr src = desc.srcBuffer->getBuffer();

    HGcontext dstContext = desc.dstBuffer->getPrimaryCtx();
    HGcontext srcContext = desc.srcBuffer->getPrimaryCtx();

    if (desc.mainstream == nullptr) {
        if (isSingleDie) {
            for (unsigned int l = 0; l < desc.loopCount; l++) {
                HG_ASSERT(hgMemcpyAsync(dst, src, desc.copySize, desc.die0stream));
            }
        } else if (desc.icnReadConfig.enableVpipeIdReadOptimize) {
            size_t die1Offset = desc.copySize / 2;
            for (unsigned int l = 0; l < desc.loopCount; l++) {
                HG_ASSERT(hgMemcpyPeerAsync( 
                    dst, 
                    dstContext,
                    src,
                    srcContext,
                    desc.copySize / 2,
                    desc.die0stream));
                HG_ASSERT(hgMemcpyPeerAsync( 
                    dst + die1Offset, 
                    dstContext,
                    src + die1Offset,
                    srcContext,
                    desc.copySize - die1Offset,
                    desc.die1stream));  
            }          
        } else {
            size_t die1Offset  = desc.copySize / 2;
            for (unsigned int l = 0; l < desc.loopCount; l++) {
                HG_ASSERT(hgMemcpyAsync(dst, src, desc.copySize / 2, desc.die0stream));
                HG_ASSERT(hgMemcpyAsync(dst + die1Offset, src + die1Offset, desc.copySize - die1Offset, desc.die1stream));
            }
        }
    } else {
        if (desc.icnReadConfig.useSingleLinkForIcnRead && sm >= 89) {
            for (unsigned int l = 0; l < desc.loopCount; l++) {
                HG_ASSERT(hgMemcpyPeerAsync( 
                    dst, 
                    dstContext,
                    src,
                    srcContext,
                    desc.copySize,
                    desc.mainstream));
            }
        } else {
            for (unsigned int l = 0; l < desc.loopCount; l++) {
                HG_ASSERT(hgMemcpyAsync(dst, src, desc.copySize, desc.mainstream));
            }
        }
    }

    return desc.copySize;
}

size_t MemcpyInitiatorCE::getAdjustedCopySize(size_t size, HGstream stream, bool use_bulk) {
    // CE does not change/truncate buffer size
    return size;
}

size_t MemcpyInitiatorSMSplitWarp::getAdjustedCopySize(size_t size, HGstream stream, bool use_bulk) {
    int numUint4 = size / sizeof(uint4);
    return numUint4 * sizeof(uint4);
}

size_t MemcpyInitiatorMulticastWrite::memcpyFunc(MemcpyDescriptor &desc) {
    return multicastCopy(desc.dstBuffer->getBuffer(), desc.srcBuffer->getBuffer(), desc.copySize, desc.mainstream, desc.loopCount);
}

size_t MemcpyInitiatorMulticastWrite::getAdjustedCopySize(size_t size, HGstream stream, bool use_bulk) {
    size = size / sizeof(unsigned);
    return size * sizeof(unsigned);
}

size_t MemcpyInitiatorSMSplitWarp::memcpyFunc(MemcpyDescriptor &desc) {
    return copyKernelSplitWarp(desc);
}

MemPtrChaseOperation::MemPtrChaseOperation(unsigned long long loopCount) : loopCount(loopCount) {
    hggcDeviceProp prop;
    HGGC_ASSERT(hggcGetDeviceProperties(&prop, 0));
    smCount = prop.multiProcessorCount;
}

double MemPtrChaseOperation::doPtrChase(const int srcId, const MemcpyBuffer &peerBuffer) {
    double lat = 0.0;
    lat = latencyPtrChaseKernel(srcId, (void*)peerBuffer.getBuffer(), peerBuffer.getBufferSize(), latencyMemAccessCnt, smCount);
    return lat;
}
