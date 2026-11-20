/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#ifndef TESTCASE_H_
#define TESTCASE_H_

#include "inline_common.h"
#include "common.h"
#include "memcpy.h"

#include <map>
#include <regex>
#include <memory>
#include <functional>
#include <utility>

// Specifies the copy initiator/engine to use.
enum class CopyInitiator {
    CE,     // Copy Engine (hgMemcpyAsync)
    SM,     // Shader/SM copy kernel
    TMA
};

// Specifies the transfer direction for host-device copies
enum class Direction {
    HostToDevice,
    DeviceToHost
};

class HostDeviceTransferDirectionStrategy {
 public:
    virtual ~HostDeviceTransferDirectionStrategy() = default;

    // Returns (src, dst) pair in correct order for the transfer direction
    virtual std::pair<const MemcpyBuffer*, const MemcpyBuffer*>
        getOrderedBuffers(const HostBuffer& host, const DeviceBuffer& device) const = 0;

    // For output label formatting
    virtual std::string getDirectionArrow() const = 0;
    virtual std::string getName() const = 0;
};

class HostToDeviceDirection : public HostDeviceTransferDirectionStrategy {
 public:
    std::pair<const MemcpyBuffer*, const MemcpyBuffer*>
    getOrderedBuffers(const HostBuffer& host, const DeviceBuffer& device) const override {
        return {&host, &device};
    }
    std::string getDirectionArrow() const override { return "->"; }
    std::string getName() const override { return "host_to_device"; }
};

class DeviceToHostDirection : public HostDeviceTransferDirectionStrategy {
 public:
    std::pair<const MemcpyBuffer*, const MemcpyBuffer*>
    getOrderedBuffers(const HostBuffer& host, const DeviceBuffer& device) const override {
        return {&device, &host};
    }
    std::string getDirectionArrow() const override { return "<-"; }
    std::string getName() const override { return "device_to_host"; }
};

// Specifies the access type for device-to-device copies
enum class AccessType {
    Read,   // Copy from peer to target (PREFER_DST_CONTEXT)
    Write   // Copy from target to peer (PREFER_SRC_CONTEXT)
};

// Strategy interface for device-to-device access type (for buffer ordering and context preference)
class DeviceToDeviceAccessStrategy {
 public:
    virtual ~DeviceToDeviceAccessStrategy() = default;

    // Returns (src, dst) pair in correct order for the access type
    virtual std::pair<const MemcpyBuffer*, const MemcpyBuffer*>
        getOrderedBuffers(const MemcpyBuffer& srcBuffer, const MemcpyBuffer& peerBuffer) const = 0;

    // Returns context preference for MemcpyOperation
    virtual ContextPreference getContextPreference() const = 0;
};

class ReadAccessStrategy : public DeviceToDeviceAccessStrategy {
 public:
    std::pair<const MemcpyBuffer*, const MemcpyBuffer*>
    getOrderedBuffers(const MemcpyBuffer& srcBuffer, const MemcpyBuffer& peerBuffer) const override {
        return {&peerBuffer, &srcBuffer};  // Read FROM peer TO src
    }
    ContextPreference getContextPreference() const override { return PREFER_DST_CONTEXT; }
};

class WriteAccessStrategy : public DeviceToDeviceAccessStrategy {
 public:
    std::pair<const MemcpyBuffer*, const MemcpyBuffer*>
    getOrderedBuffers(const MemcpyBuffer& srcBuffer, const MemcpyBuffer& peerBuffer) const override {
        return {&srcBuffer, &peerBuffer};  // Write FROM src TO peer
    }
    ContextPreference getContextPreference() const override { return PREFER_SRC_CONTEXT; }
};

// Holds a pair of buffers for src and peer devices
class BufferPair {
 public:
    std::unique_ptr<MemcpyBuffer> src;
    std::unique_ptr<MemcpyBuffer> peer;

    BufferPair(std::unique_ptr<MemcpyBuffer> s, std::unique_ptr<MemcpyBuffer> p)
        : src(std::move(s)), peer(std::move(p)) {}
};

// Abstract interface for buffer type-specific operations
class BufferTypeStrategy {
 public:
    virtual ~BufferTypeStrategy() = default;

    // Check if this buffer type is supported on the system
    virtual bool isSupported() const = 0;

    // Create a pair of buffers for the given devices
    virtual std::unique_ptr<BufferPair> createBufferPair(
        size_t size, int srcDeviceId, int peerDeviceId) const = 0;

    // Enable peer access between buffers, returns false if not possible
    virtual bool enablePeerAccess(BufferPair& pair) const = 0;

    // Label for output/error messages
    virtual std::string getLabel() const = 0;
};

// Implementation for standard DeviceBuffer
class DeviceBufferStrategy : public BufferTypeStrategy {
 public:
    bool isSupported() const override { return true; }

    std::unique_ptr<BufferPair> createBufferPair(
            size_t size, int srcDeviceId, int peerDeviceId) const override {
        return std::make_unique<BufferPair>(
            std::unique_ptr<MemcpyBuffer>(new DeviceBuffer(size, srcDeviceId)),
            std::unique_ptr<MemcpyBuffer>(new DeviceBuffer(size, peerDeviceId)));
    }

    bool enablePeerAccess(BufferPair& pair) const override {
        return static_cast<DeviceBuffer&>(*pair.src).enablePeerAccess(static_cast<DeviceBuffer&>(*pair.peer));
    }

    std::string getLabel() const override { return "Device"; }
};

// Read-only peer mappings keep PPU CE reads from being converted into writes.
class DeviceBufferVmStrategy : public BufferTypeStrategy {
 public:
    bool isSupported() const override { return true; }

    std::unique_ptr<BufferPair> createBufferPair(
            size_t size, int srcDeviceId, int peerDeviceId) const override {
        return std::make_unique<BufferPair>(
            std::unique_ptr<MemcpyBuffer>(new DeviceBufferVm(size, srcDeviceId)),
            std::unique_ptr<MemcpyBuffer>(new DeviceBufferVm(size, peerDeviceId)));
    }

    bool enablePeerAccess(BufferPair& pair) const override {
        return static_cast<DeviceBufferVm&>(*pair.src).enableReadOnlyAccessFromPeer(static_cast<DeviceBufferVm&>(*pair.peer));
    }

    std::string getLabel() const override { return "Device VM"; }
};

/* Carries waiver reason when a test cannot run on the current system */
struct [[nodiscard]] FilterResult {
    bool passed;
    std::string reason;
    explicit operator bool() const { return passed; }
    static FilterResult pass() { return {true, ""}; }
    static FilterResult waive(std::string reason) { return {false, std::move(reason)}; }
    // Note: overloaded operator&& does not short-circuit; both operands are always
    // evaluated.
    friend FilterResult operator&&(FilterResult a, FilterResult b) { return a ? b : a; }
};

class Testcase {
 protected:
    std::string key;
    std::string desc;

    // Type alias for all-to-one and one-to-all buffer pairing callback
    using allToOneOneToAllCallback = std::function<void(int oneDeviceId,
                                                         std::vector<const MemcpyBuffer*>& allBuffers,
                                                         std::vector<const MemcpyBuffer*>& oneBuffers)>;

    static FilterResult filterHasAccessiblePeerPairs();
    static FilterResult filterSupportsMulticast();
    static FilterResult filterSupportsTMA();
#ifdef MULTINODE
    static FilterResult filterHasMultiplePPUsMultinode();
#endif
    // helper functions
    void forEachAllToOneBufferPairing(unsigned long long size, MemcpyOperation &memcpyInstance, bool isAllToOne, allToOneOneToAllCallback callback);
    void allToOneHelper(unsigned long long size, MemcpyOperation &memcpyInstance, PeerValueMatrix<double> &bandwidthValues);
    void oneToAllHelper(unsigned long long size, MemcpyOperation &memcpyInstance, PeerValueMatrix<double> &bandwidthValues);
    void allHostHelper(unsigned long long size, MemcpyOperation &memcpyInstance, PeerValueMatrix<double> &bandwidthValues, bool sourceIsHost);
    void allHostBidirHelper(unsigned long long size, MemcpyOperation &memcpyInstance, PeerValueMatrix<double> &bandwidthValues, bool sourceIsHost);

    // Copy the worst run-to-run coefficient of variation measured by the
    // operation onto the result matrix, so it is emitted as the test's
    // COEFFICIENT_OF_VARIATION stability metric. No-op if nothing was measured.
    static void recordStability(PeerValueMatrix<double> &bandwidthValues, const MemcpyOperation &memcpyInstance) {
        double cv = memcpyInstance.worstCoefficientOfVariation();
        if (cv >= 0.0) {
            bandwidthValues.coefficientOfVariation = cv;
        }
    }

    template<typename AlignedLatencyNode = struct LatencyNode>
    void latencyHelper(const MemcpyBuffer &dataBuffer, bool measureDeviceToDeviceLatency);

    static std::map<std::string, std::map<std::string, std::shared_ptr<PeerValueMatrix<double>>>> globalTestCache;

    static std::string getComplementaryCacheKey(const std::string& testKey) {
        static std::vector<std::pair<std::string, std::string>> pairs = {
            {"all_to_host", "host_to_all"},
            {"host_to_device", "device_to_host"},
            {"_read", "_write"}
        };

        for (const auto& pair : pairs) {
            if (testKey.find(pair.first) != std::string::npos) {
                return std::regex_replace(testKey, std::regex(pair.first), pair.second);
            } else if (testKey.find(pair.second) != std::string::npos) {
                return std::regex_replace(testKey, std::regex(pair.second), pair.first);
            }
        }
        return "";
    }

    template<typename TestExecutor>
    void runWithCache(PeerValueMatrix<double>& result, TestExecutor executor) {
        std::string complementaryKey = getComplementaryCacheKey(key);
        if (gSettings.performanceCacheEnabled() && !complementaryKey.empty() && globalTestCache.count(complementaryKey) &&
            globalTestCache[complementaryKey].count(key) &&
            globalTestCache[complementaryKey][key] != nullptr) {
            result = *globalTestCache[complementaryKey][key];
            return;
        }

        executor();

        globalTestCache[key][key] = std::make_shared<PeerValueMatrix<double>>(result);
    }

    // Symmetric caching for device-to-device pairs
    template<typename TestExecutor>
    void cachedPairLoop(const std::vector<std::pair<int, int>>& pairs,
                       PeerValueMatrix<double>& matrix1,
                       PeerValueMatrix<double>& matrix2,
                       PeerValueMatrix<double>& matrixTotal,
                       TestExecutor executor) {
        for (const auto& pair : pairs) {
            int src = pair.first, dst = pair.second;

            // Reversing the pair swaps the two directional results.
            if (gSettings.performanceCacheEnabled() && src > dst && matrix1.value(dst, src).has_value()) {
                matrix1.value(src, dst) = matrix2.value(dst, src);
                matrix2.value(src, dst) = matrix1.value(dst, src);
                matrixTotal.value(src, dst) = matrixTotal.value(dst, src);
                continue;
            }

            auto results = executor(src, dst);
            if (results.empty()) {
                continue;
            }
            ASSERT(results.size() == 2);

            matrix1.value(src, dst) = results[0];
            matrix2.value(src, dst) = results[1];
            matrixTotal.value(src, dst) = results[0] + results[1];
        }
    }

 public:
    Testcase(std::string key, std::string desc);
    virtual ~Testcase() {}

    std::string testKey();
    std::string testDesc();

    // Returns true if the testcase can be run on the current system
    virtual FilterResult filter() {
        return FilterResult::pass();
    }

    // Runs the testcase
    virtual void run(unsigned long long size, unsigned long long loopCount) = 0;
};

// Filter source and peer device ordinals only when both --pair endpoints are locally visible.
inline bool isPpuPairEndpoint(int deviceId) {
    if (!hasPpuPair()) return true;
    if (ppuPairDev0 < 0 || ppuPairDev1 < 0) return true;
    return deviceId == ppuPairDev0 || deviceId == ppuPairDev1;
}

// Host-Device transfer test
class HostDeviceTransfer : public Testcase {
    CopyInitiator initiator_;
    std::unique_ptr<HostDeviceTransferDirectionStrategy> directionStrategy_;

 public:
    HostDeviceTransfer(CopyInitiator initiator, Direction direction);
    virtual ~HostDeviceTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;
    FilterResult filter() override;

 private:
    MemcpyInitiator* createMemcpyInitiator() const;
    static std::unique_ptr<HostDeviceTransferDirectionStrategy> createDirectionStrategy(Direction dir);
    static std::string generateKey(CopyInitiator initiator, Direction direction);
    static std::string generateDesc(CopyInitiator initiator, Direction direction);
    std::string buildOutputLabel() const;
};

// Host-Device bidirectional transfer test (CE/SM)
class HostDeviceBidirTransfer : public Testcase {
    CopyInitiator initiator_;
    Direction direction_;

 public:
    HostDeviceBidirTransfer(CopyInitiator initiator, Direction direction);
    virtual ~HostDeviceBidirTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;

 private:
    MemcpyInitiator* createMemcpyInitiator() const;
    static std::string generateKey(CopyInitiator initiator, Direction direction);
    static std::string generateDesc(CopyInitiator initiator, Direction direction);
    std::string buildOutputLabel() const;
};

// Device-to-device transfer test (CE/SM) - Read/Write
class DeviceToDeviceTransfer : public Testcase {
    CopyInitiator initiator_;
    AccessType accessType_;
    std::unique_ptr<DeviceToDeviceAccessStrategy> accessStrategy_;
    std::unique_ptr<BufferTypeStrategy> bufferStrategy_;

 public:
    DeviceToDeviceTransfer(CopyInitiator initiator, AccessType accessType);
    virtual ~DeviceToDeviceTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;
    FilterResult filter() override;

 private:
    MemcpyInitiator* createMemcpyInitiator() const;
    std::unique_ptr<BufferTypeStrategy> createBufferStrategy() const;
    static std::unique_ptr<DeviceToDeviceAccessStrategy> createAccessStrategy(AccessType accessType);
    static std::string generateKey(CopyInitiator initiator, AccessType accessType);
    static std::string generateDesc(CopyInitiator initiator, AccessType accessType);
    std::string buildOutputLabel() const;
};

// Device-to-device bidirectional transfer test (CE/SM) - Read/Write
class DeviceToDeviceBidirTransfer : public Testcase {
    CopyInitiator initiator_;
    AccessType accessType_;
    std::unique_ptr<DeviceToDeviceAccessStrategy> accessStrategy_;
    std::unique_ptr<BufferTypeStrategy> bufferStrategy_;

 public:
    DeviceToDeviceBidirTransfer(CopyInitiator initiator, AccessType accessType);
    virtual ~DeviceToDeviceBidirTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;
    FilterResult filter() override;

 private:
    MemcpyInitiator* createMemcpyInitiator() const;
    std::unique_ptr<BufferTypeStrategy> createBufferStrategy() const;
    static std::unique_ptr<DeviceToDeviceAccessStrategy> createAccessStrategy(AccessType accessType);
    static std::string generateKey(CopyInitiator initiator, AccessType accessType);
    static std::string generateDesc(CopyInitiator initiator, AccessType accessType);
    std::string buildOutputLabel(const std::string& suffix) const;
};

// Device local copy transfer test
class DeviceLocalCopyTransfer : public Testcase {
    CopyInitiator initiator_;

 public:
    explicit DeviceLocalCopyTransfer(CopyInitiator initiator);
    virtual ~DeviceLocalCopyTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;
    FilterResult filter() override;

 private:
    MemcpyInitiator* createMemcpyInitiator() const;
    static std::string generateKey(CopyInitiator initiator);
    static std::string generateDesc(CopyInitiator initiator);
    std::string buildOutputLabel() const;
};

class DeviceLocalReadSM : public Testcase {
 public:
    DeviceLocalReadSM() : Testcase("device_local_read_sm", "\tMeasures bandwidth of SM read-only access to local device memory.") {
    }
    void run(unsigned long long size, unsigned long long loopCount) override;
};

class DeviceLocalWriteSM : public Testcase {
 public:
    DeviceLocalWriteSM() : Testcase("device_local_write_sm", "\tMeasures bandwidth of SM write-only access to local device memory.") {
    }
    void run(unsigned long long size, unsigned long long loopCount) override;
};

// All-to-host transfer test
class AllToHostTransfer : public Testcase {
    CopyInitiator initiator_;

 public:
    explicit AllToHostTransfer(CopyInitiator initiator);
    virtual ~AllToHostTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;

 private:
    MemcpyInitiator* createMemcpyInitiator() const;
    static std::string generateKey(CopyInitiator initiator);
    static std::string generateDesc(CopyInitiator initiator);
    std::string buildOutputLabel() const;
};

// All-to-host bidirectional transfer test
class AllToHostBidirTransfer : public Testcase {
    CopyInitiator initiator_;

 public:
    explicit AllToHostBidirTransfer(CopyInitiator initiator);
    virtual ~AllToHostBidirTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;

 private:
    MemcpyInitiator* createMemcpyInitiator() const;
    static std::string generateKey(CopyInitiator initiator);
    static std::string generateDesc(CopyInitiator initiator);
    std::string buildOutputLabel() const;
};

// Host-to-all transfer test
class HostToAllTransfer : public Testcase {
    CopyInitiator initiator_;

 public:
    explicit HostToAllTransfer(CopyInitiator initiator);
    virtual ~HostToAllTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;

 private:
    MemcpyInitiator* createMemcpyInitiator() const;
    static std::string generateKey(CopyInitiator initiator);
    static std::string generateDesc(CopyInitiator initiator);
    std::string buildOutputLabel() const;
};

// Host-to-all bidirectional transfer test
class HostToAllBidirTransfer : public Testcase {
    CopyInitiator initiator_;

 public:
    explicit HostToAllBidirTransfer(CopyInitiator initiator);
    virtual ~HostToAllBidirTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;

 private:
    MemcpyInitiator* createMemcpyInitiator() const;
    static std::string generateKey(CopyInitiator initiator);
    static std::string generateDesc(CopyInitiator initiator);
    std::string buildOutputLabel() const;
};

// All-to-one transfer test
class AllToOneTransfer : public Testcase {
    CopyInitiator initiator_;
    AccessType accessType_;
    std::unique_ptr<DeviceToDeviceAccessStrategy> accessStrategy_;

 public:
    AllToOneTransfer(CopyInitiator initiator, AccessType accessType);
    virtual ~AllToOneTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;
    FilterResult filter() override;

 private:
    MemcpyInitiator* createMemcpyInitiator() const;
    static std::unique_ptr<DeviceToDeviceAccessStrategy> createAccessStrategy(AccessType accessType);
    static std::string generateKey(CopyInitiator initiator, AccessType accessType);
    static std::string generateDesc(CopyInitiator initiator, AccessType accessType);
    std::string buildOutputLabel() const;
};

// One-to-all transfer test
class OneToAllTransfer : public Testcase {
    CopyInitiator initiator_;
    AccessType accessType_;
    std::unique_ptr<DeviceToDeviceAccessStrategy> accessStrategy_;

 public:
    OneToAllTransfer(CopyInitiator initiator, AccessType accessType);
    virtual ~OneToAllTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;
    FilterResult filter() override;

 private:
    MemcpyInitiator* createMemcpyInitiator() const;
    static std::unique_ptr<DeviceToDeviceAccessStrategy> createAccessStrategy(AccessType accessType);
    static std::string generateKey(CopyInitiator initiator, AccessType accessType);
    static std::string generateDesc(AccessType accessType);
    std::string buildOutputLabel() const;
};

// SM Testcase classes
// Host to device SM latency using a ptr chase kernel
class HostDeviceLatencySM: public Testcase {
 public:
    HostDeviceLatencySM() : Testcase("host_device_latency_sm",
            "\tHost - device access latency using a pointer chase kernel\n"
            "\tA 2MB buffer is allocated on the host and is accessed by the PPU") {}
    virtual ~HostDeviceLatencySM() {}
    void run(unsigned long long size, unsigned long long loopCount);
};

// Device to Device SM Latency ptr chase kernel
class DeviceToDeviceLatencySM: public Testcase {
 public:
    DeviceToDeviceLatencySM() : Testcase("device_to_device_latency_sm",
            "\tMeasures latency of a pointer dereference operation between each pair of accessible peers.\n"
            "\tA 2MB buffer is allocated on a PPU and is accessed by the peer PPU to determine latency.\n"
            "\t--bufferSize flag is ignored") {}
    virtual ~DeviceToDeviceLatencySM() {}
    void run(unsigned long long size, unsigned long long loopCount);
    FilterResult filter() {
        if (deviceCount > 0 && gSettings.testLocalCopy && !hasPpuPair()) {
            return FilterResult::pass();
        }
        return Testcase::filterHasAccessiblePeerPairs();
    }
};

class DeviceToDeviceLatencyTMA: public Testcase {
 public:
    DeviceToDeviceLatencyTMA() : Testcase("device_to_device_latency_tma",
            "\tMeasures latency of a pointer dereference operation between each pair of accessible peers using TMA.\n"
            "\tA 2MB buffer is allocated on a PPU and is accessed by the peer PPU using TMA to determine latency.\n"
            "\t--bufferSize flag is ignored") {}
    virtual ~DeviceToDeviceLatencyTMA() {}
    void run(unsigned long long size, unsigned long long loopCount) override;
    FilterResult filter() override {
        auto r = Testcase::filterHasAccessiblePeerPairs();
        if (!r) return r;
        return Testcase::filterSupportsTMA();
    }
};

#ifdef MULTINODE
// Multinode device-to-device transfer test (CE/SM/TMA) - Read/Write
class MultinodeDeviceToDeviceTransfer : public Testcase {
    CopyInitiator initiator_;
    AccessType accessType_;

 public:
    MultinodeDeviceToDeviceTransfer(CopyInitiator initiator, AccessType accessType);
    virtual ~MultinodeDeviceToDeviceTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;
    FilterResult filter() override;

 private:
    ContextPreference getContextPreference() const;
    MemcpyInitiator* createMemcpyInitiator() const;
    static std::string generateKey(CopyInitiator initiator, AccessType accessType);
    static std::string generateDesc(CopyInitiator initiator, AccessType accessType);
    std::string buildOutputLabel() const;
};

// Multinode device-to-device bidirectional transfer test (CE/SM) - Read/Write
class MultinodeDeviceToDeviceBidirTransfer : public Testcase {
    CopyInitiator initiator_;
    AccessType accessType_;

 public:
    MultinodeDeviceToDeviceBidirTransfer(CopyInitiator initiator, AccessType accessType);
    virtual ~MultinodeDeviceToDeviceBidirTransfer() {}
    void run(unsigned long long size, unsigned long long loopCount) override;
    FilterResult filter() override;

 private:
    ContextPreference getContextPreference() const;
    MemcpyInitiator* createMemcpyInitiator() const;
    static std::string generateKey(CopyInitiator initiator, AccessType accessType);
    static std::string generateDesc(CopyInitiator initiator, AccessType accessType);
    std::string buildOutputLabel(const std::string& suffix) const;
};

class MultinodeAllToOneWriteSM: public Testcase {
 public:
    MultinodeAllToOneWriteSM() : Testcase("multinode_device_to_device_all_to_one_write_sm",
            "\tMeasures the total bandwidth of copies from all accessible peers to a single device, for each\n"
            "\tdevice. Bandwidth is reported as the total inbound bandwidth for each device.\n"
            "\tWrite tests launch a copy from the peer to the target device using the peer's context.") {}
    virtual ~MultinodeAllToOneWriteSM() {}
    void run(unsigned long long size, unsigned long long loopCount);
    FilterResult filter() {
        return Testcase::filterHasMultiplePPUsMultinode();
    }
};

class MultinodeAllFromOneReadSM: public Testcase {
 public:
    MultinodeAllFromOneReadSM() : Testcase("multinode_device_to_device_all_from_one_read_sm",
            "\tMeasures the total bandwidth of copies from a single device to all accessible peers, for each\n"
            "\tdevice. Bandwidth is reported as the total outbound bandwidth for each device.\n"
            "\tRead tests launch a copy from the target device to the peer using the peer's context.") {}
    virtual ~MultinodeAllFromOneReadSM() {}
    void run(unsigned long long size, unsigned long long loopCount);
    FilterResult filter() {
        return Testcase::filterHasMultiplePPUsMultinode();
    }
};

class MultinodeBroadcastOneToAllSM: public Testcase {
 public:
    MultinodeBroadcastOneToAllSM() : Testcase("multinode_device_to_device_broadcast_one_to_all_sm",
            "\tMeasures bandwidth of a copy kernel copying data from device memory to multicast allocated memory\n"
            "\tthat's mapped on all accessible peers.\n"
            "\tTests launch a copy from the target device to the multicast memory on target using the target's context.") {}
    virtual ~MultinodeBroadcastOneToAllSM() {}
    void run(unsigned long long size, unsigned long long loopCount);
    FilterResult filter() {
        return Testcase::filterHasMultiplePPUsMultinode() && Testcase::filterSupportsMulticast();
    }
};

class MultinodeBroadcastAllToAllSM: public Testcase {
 public:
    MultinodeBroadcastAllToAllSM() : Testcase("multinode_device_to_device_broadcast_all_to_all_sm",
            "\tMeasures bandwidth of copy kernels copying data from device memory to multicast allocated memory\n"
            "\tthat's mapped on all accessible peers.\n"
            "\tAll devices are doing copies at the same time.\n"
            "\tTests launch copies from the target device to the multicast memory on target using the target's context.") {}
    virtual ~MultinodeBroadcastAllToAllSM() {}
    void run(unsigned long long size, unsigned long long loopCount);
    FilterResult filter() {
        return Testcase::filterHasMultiplePPUsMultinode() && Testcase::filterSupportsMulticast();
    }
};

// TODO(pgumienny) - add remaining combination of Read/Write CE/SM once the tooling is in
class MultinodeBisectWriteCE: public Testcase {
 public:
    MultinodeBisectWriteCE() : Testcase("multinode_bisect_write_ce",
            "\tMeasures bandwidths of simultaneous copies.\n"
            "\tFor an N-PPU system, N copies occur at the same time.\n"
            "\tPPU owned by rank A writes to PPU owned by rank (A + N/2) % N.\n") {}
    virtual ~MultinodeBisectWriteCE() {}
    void run(unsigned long long size, unsigned long long loopCount);
    FilterResult filter() { return Testcase::filterHasMultiplePPUsMultinode(); }
};

class MultinodeMulticastFabricPut : public Testcase {
 public:
    explicit MultinodeMulticastFabricPut(CopyInitiator initiator);
    virtual ~MultinodeMulticastFabricPut() {}
    void run(unsigned long long size, unsigned long long loopCount) override;
    FilterResult filter() override;

 private:
    CopyInitiator initiator_;
    std::string buildOutputLabel() const;
};
#endif

#endif  // TESTCASE_H_
