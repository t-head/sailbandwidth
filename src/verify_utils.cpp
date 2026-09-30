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

#include "verify_utils.h"

#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits.h>
#include <ostream>
#include <vector>

#include "error_handling.h"
#include "output.h"

namespace {

const unsigned long long kDefaultDumpSize = 100;  // bad values listed when SAILBW_DUMP_SIZE is unset
const unsigned long long kDumpSizeAll = ~0ULL;    // SAILBW_DUMP_SIZE=ALL sentinel
const size_t kMaxDumpReserve = 4096;              // cap of the up-front mismatch-list reservation
const unsigned long long kChunkElems = (128ULL * 1024 * 1024) / sizeof(unsigned int);  // host re-read chunk
const int kLabelWidth = 17;                       // aligns report / dump-header labels

struct VerifyMismatch {
    unsigned long long offset;
    unsigned int expected;
    unsigned int actual;
    unsigned int srcValue;   // src content before the transfer
    unsigned int initValue;  // verified buffer's pre-fill content
};

// Outcome of the host re-scan of the endpoint buffers.
struct VerifyScan {
    unsigned long long total = 0;
    unsigned long long firstOffset = 0;
    unsigned int flippedEver = 0;        // bits wrong in at least one bad value
    unsigned int flippedAlways = ~0u;    // bits wrong in every bad value
    unsigned long long srcOk = 0;        // source content verdict counts
    unsigned long long srcBad = 0;
    unsigned long long xferOk = 0;       // transfer verdict counts
    unsigned long long xferBad = 0;
    unsigned long long notWritten = 0;   // still holds the pre-fill sentinel
    bool srcReadable = false;            // source content is readable from its owning device
    // True only when this process owns both endpoints, i.e. source and transfer can be told apart.
    // A multi-process (multinode) run always has one endpoint remote, so it stays false and only the
    // top-level received-vs-expected comparison is reported.
    bool attributable = false;
    bool haveInitPattern = false;
    std::vector<VerifyMismatch> listed;
};

std::string hex8(unsigned int value) {
    std::stringstream s;
    s << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return s.str();
}

// Whether the SAILBW_VERIFY=<srcId>,<dstId> fault-injection request targets this link.
bool faultMatchesLink(int linkSrcId, int linkDstId) {
    const char* spec = getenv("SAILBW_VERIFY");
    if (spec == nullptr) {
        return false;
    }
    char* endp = nullptr;
    long injSrc = strtol(spec, &endp, 10);
    if (endp == nullptr || *endp != ',') {
        return false;
    }
    long injDst = strtol(endp + 1, nullptr, 10);
    return injSrc == linkSrcId && injDst == linkDstId;
}

#ifdef MULTINODE
std::string lookupByRank(const std::vector<std::string>& table, int rank) {
    if (rank < 0 || static_cast<size_t>(rank) >= table.size() || table[rank].empty()) {
        return "n/a";
    }
    return table[rank];
}
#endif

// PCI BDF of a link endpoint ("n/a" for host / unknown).
std::string endpointBdf(const MemcpyBuffer* buf) {
    if (!buf->isDeviceBuffer()) {
        return "n/a";
    }
#ifdef MULTINODE
    if (buf->getMPIRank() >= 0) {
        return lookupByRank(ppuBDFByRank, buf->getMPIRank());
    }
#endif
    return getDeviceBDF(buf->getBufferIdx());
}

std::string endpointHost(const MemcpyBuffer* buf) {
#ifdef MULTINODE
    if (buf->getMPIRank() >= 0) {
        return lookupByRank(ppuHostByRank, buf->getMPIRank());
    }
#endif
    return getLocalHostname();
}

std::ostream& field(std::ostream& os, const char* prefix, const char* label) {
    return os << prefix << std::left << std::setw(kLabelWidth) << label << std::right << ": ";
}

// Whether this endpoint's memory can be read back from the device that owns the allocation. A read of
// a buffer owned elsewhere returns what a remote read gives, which cannot attribute a fault.
bool ownedLocally(const MemcpyBuffer* buf) {
    if (!buf->isDeviceBuffer()) {
        return true;  // host memory, read by the CPU
    }
#ifdef MULTINODE
    if (buf->getMPIRank() >= 0 && buf->getMPIRank() != worldRank) {
        return false;  // another rank owns it; every access travels over the fabric
    }
#endif
    return buf->getPrimaryCtx() != nullptr;
}

// Makes an endpoint's owning context current for the duration of the scope and restores the previous
// one afterwards, so a read of that endpoint is issued by the device that owns it.
class OwnerContextGuard {
 public:
    explicit OwnerContextGuard(const MemcpyBuffer* buf) {
        HGcontext owner = buf->getPrimaryCtx();
        if (owner == nullptr) {
            return;
        }
        HG_ASSERT(hgCtxGetCurrent(&previousCtx));
        if (owner != previousCtx) {
            HG_ASSERT(hgCtxSetCurrent(owner));
            restore = true;
        }
    }

    ~OwnerContextGuard() {
        if (restore) {
            HG_ASSERT(hgCtxSetCurrent(previousCtx));
        }
    }

    OwnerContextGuard(const OwnerContextGuard&) = delete;
    OwnerContextGuard& operator=(const OwnerContextGuard&) = delete;

 private:
    HGcontext previousCtx = nullptr;
    bool restore = false;
};

// Copies a slice of an endpoint's buffer into host memory through its owning context.
void readEndpointChunk(const MemcpyBuffer* buf, HGdeviceptr base, unsigned long long byteOffset,
                       void* host, size_t bytes) {
    OwnerContextGuard ownerCtx(buf);
    HG_ASSERT(hgMemcpyAsync(reinterpret_cast<HGdeviceptr>(host),
                            reinterpret_cast<HGdeviceptr>(reinterpret_cast<char*>(base) + byteOffset),
                            bytes, HG_STREAM_PER_THREAD));
    HG_ASSERT(hgStreamSynchronize(HG_STREAM_PER_THREAD));
}

// Identity of the failing transfer (console report and dump header).
void writeIdentity(std::ostream& os, const char* prefix, const MemcpyBuffer* srcBuf,
                   const MemcpyBuffer* dstBuf) {
    field(os, prefix, "testcase") << currentTestcaseName << "\n";
    field(os, prefix, "transport") << srcBuf->getBufferString() << " -> "
                                   << dstBuf->getBufferString() << "\n";
    field(os, prefix, "src") << "host=" << endpointHost(srcBuf) << "  rank=" << verifyLinkId(srcBuf)
                             << "  BDF=" << endpointBdf(srcBuf) << "\n";
    field(os, prefix, "dst") << "host=" << endpointHost(dstBuf) << "  rank=" << verifyLinkId(dstBuf)
                             << "  BDF=" << endpointBdf(dstBuf) << "\n";
#ifdef MULTINODE
    field(os, prefix, "reported by") << "host=" << localHostname << "  rank=" << worldRank << "\n";
#endif
}

// Mismatch statistics (console report and dump header).
void writeStats(std::ostream& os, const char* prefix, const VerifyScan& scan,
                unsigned long long numElements) {
    double totalMiB = static_cast<double>(numElements) * sizeof(unsigned int) / (1024.0 * 1024.0);
    double pct = 100.0 * static_cast<double>(scan.total) / static_cast<double>(numElements);
    field(os, prefix, "total values") << numElements << " (" << std::fixed << std::setprecision(2)
                                      << totalMiB << " MiB)\n";
    field(os, prefix, "bad values") << scan.total << " (" << std::setprecision(4) << pct << "%)\n";
    field(os, prefix, "flipped bits")
        << "ever=" << hex8(scan.flippedEver) << " always=" << hex8(scan.flippedAlways)
        << (scan.flippedEver == scan.flippedAlways ? "  (the same bit(s) in every bad value)" : "")
        << "\n";
    // The source/transfer split is only meaningful when this process owns both endpoints; otherwise
    // the report stops at the received-vs-expected comparison above.
    if (scan.attributable) {
        field(os, prefix, "source content") << "OK=" << scan.srcOk << " BAD=" << scan.srcBad << "\n";
        field(os, prefix, "transfer") << "OK=" << scan.xferOk << " BAD=" << scan.xferBad;
        if (scan.haveInitPattern) {
            os << " NOT-WRITTEN=" << scan.notWritten;
        }
        os << "\n";
    }
}

// Per-value verdicts: did src hold the expected pattern, and did the transfer preserve it?
const char* srcVerdict(const VerifyScan& scan, const VerifyMismatch& m) {
    if (!scan.srcReadable) {
        return "n/a";
    }
    return m.srcValue == m.expected ? "OK" : "BAD";
}

const char* transferVerdict(const VerifyScan& scan, const VerifyMismatch& m) {
    if (scan.haveInitPattern && m.actual == m.initValue) {
        return "NOT-WR";
    }
    if (!scan.srcReadable) {
        return "n/a";
    }
    return m.actual == m.srcValue ? "OK" : "BAD";
}

// Re-read both endpoints chunk by chunk, keeping the first listLimit mismatches for the dump. A
// device buffer is read through its owning context, so the values are what the two memories hold.
VerifyScan scanVerifiedBuffer(const MemcpyBuffer* srcBuf, const MemcpyBuffer* verifiedBuf,
                              HGdeviceptr buffer,
                              const unsigned int* hostPattern, const unsigned int* hostInitPattern,
                              unsigned long long numElements, unsigned int numPatternElements,
                              unsigned long long listLimit) {
    VerifyScan scan;
    scan.haveInitPattern = hostInitPattern != nullptr;
    // The source is dropped from the comparison when it has no local mapping, when another rank owns
    // it, or when it is the verified buffer itself (the bidirectional split-warp copy rewrote it).
    HGdeviceptr srcBuffer = srcBuf->getBuffer();
    scan.srcReadable = srcBuffer != static_cast<HGdeviceptr>(0) && srcBuffer != buffer &&
                       ownedLocally(srcBuf);
    // Source vs transfer can only be told apart when this process can independently read the source
    // and verified destination. A remote, unmapped, or self-verified source reports only received
    // vs expected.
    scan.attributable = scan.srcReadable && ownedLocally(verifiedBuf);
    if (listLimit != kDumpSizeAll) {
        scan.listed.reserve(std::min<size_t>(kMaxDumpReserve, static_cast<size_t>(listLimit)));
    }

    std::vector<unsigned int> chunk(std::min<unsigned long long>(kChunkElems, numElements));
    std::vector<unsigned int> srcChunk(scan.attributable ? chunk.size() : 0);
    for (unsigned long long base = 0; base < numElements; base += kChunkElems) {
        unsigned long long chunkElems = std::min<unsigned long long>(kChunkElems, numElements - base);
        unsigned long long byteOffset = base * sizeof(unsigned int);
        size_t chunkBytes = static_cast<size_t>(chunkElems * sizeof(unsigned int));

        readEndpointChunk(verifiedBuf, buffer, byteOffset, chunk.data(), chunkBytes);
        if (scan.attributable) {
            readEndpointChunk(srcBuf, srcBuffer, byteOffset, srcChunk.data(), chunkBytes);
        }

        for (unsigned long long k = 0; k < chunkElems; k++) {
            unsigned long long offset = base + k;
            unsigned int expected = hostPattern[offset % numPatternElements];
            if (chunk[k] == expected) {
                continue;
            }
            VerifyMismatch m = {offset, expected, chunk[k],
                                scan.attributable ? srcChunk[k] : 0u,
                                scan.haveInitPattern ? hostInitPattern[offset % numPatternElements] : 0u};
            if (scan.total == 0) {
                scan.firstOffset = offset;
            }
            scan.total++;
            scan.flippedEver |= expected ^ m.actual;
            scan.flippedAlways &= expected ^ m.actual;
            if (scan.attributable) {
                if (m.srcValue == expected) scan.srcOk++; else scan.srcBad++;
                if (scan.haveInitPattern && m.actual == m.initValue) {
                    scan.notWritten++;
                } else if (m.actual == m.srcValue) {
                    scan.xferOk++;
                } else {
                    scan.xferBad++;
                }
            }
            if (scan.listed.size() < listLimit) {
                scan.listed.push_back(m);
            }
        }
    }
    return scan;
}

// SAILBW_DUMP_SIZE=<N|ALL>, defaulting to kDefaultDumpSize.
unsigned long long parseDumpSize() {
    const char* spec = getenv("SAILBW_DUMP_SIZE");
    if (spec == nullptr) {
        return kDefaultDumpSize;
    }
    if (strcasecmp(spec, "ALL") == 0) {
        return kDumpSizeAll;
    }
    char* endp = nullptr;
    long value = strtol(spec, &endp, 10);
    return (endp != spec && value > 0) ? static_cast<unsigned long long>(value) : kDefaultDumpSize;
}

// Resolve SAILBW_DUMP_PATH (possibly empty or relative) to an absolute directory; each rank writes
// to its own node's filesystem.
std::string resolveDumpDir(const char* spec) {
    std::string dir = spec[0] != '\0' ? std::string(spec) : std::string(".");
    char cwd[PATH_MAX];
    if (dir[0] == '/' || getcwd(cwd, sizeof(cwd)) == nullptr) {
        return dir;
    }
    return dir == "." ? std::string(cwd) : std::string(cwd) + "/" + dir;
}

// Timestamped, per-rank dump file name.
std::string makeDumpPath(const std::string& dir, int ppuId, unsigned long long firstOffset) {
    char timestamp[32] = {0};
    std::time_t now = std::time(nullptr);
    std::tm local = {};
    if (localtime_r(&now, &local) != nullptr) {
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local);
    }
    std::stringstream name;
    name << dir << "/sailbw_dump_ppu" << ppuId;
#ifdef MULTINODE
    name << "_rank" << worldRank;
#endif
    name << "_off" << firstOffset << "_" << timestamp << ".txt";
    return name.str();
}

void writeDumpLegend(std::ostream& f, bool attributable) {
    f << "#\n"
      << "# each value is a 32-bit word; offset is the word index (byte = offset * 4)\n";
    if (!attributable) {
        f << "# columns: expected = value this word should have; actual = value received;\n"
          << "#          xor = expected^actual\n"
          << "#\n";
        return;
    }
    f << "# columns: expected = value this word should have; actual = value received;\n"
      << "#          src_content = value the source holds; xor = expected^actual; source/transfer\n"
      << "#          = where the problem is (see below). Each buffer is read from the device that\n"
      << "#          owns it, so the result does not depend on the testcase direction\n"
      << "#\n"
      << "# how to read:\n"
      << "#   source=OK  transfer=BAD    -> the source was correct but a wrong value arrived: the\n"
      << "#                                 problem is the link, or the destination memory\n"
      << "#   source=OK  transfer=NOT-WR -> the destination retained its prefill marker;\n"
      << "#                                 consistent with an unwritten destination\n"
      << "#   source=BAD transfer=OK     -> the source was already wrong and was copied unchanged:\n"
      << "#                                 the link is not implicated by this evidence;\n"
      << "#                                 the source is the problem\n"
      << "#   source=BAD transfer=BAD    -> the source is wrong and a different wrong value arrived\n"
      << "#   flipped bits ever/always   -> ever: bits wrong in at least one value; always: bits\n"
      << "#                                 wrong in every value\n"
      << "#\n";
}

// Write the mismatch dump; returns the file path, or an empty string on failure.
std::string writeDumpFile(const char* dumpSpec, const MemcpyBuffer* srcBuf,
                          const MemcpyBuffer* dstBuf, const MemcpyBuffer* verifiedBuf,
                          HGdeviceptr buffer, const VerifyScan& scan,
                          unsigned long long numElements) {
    std::string path = makeDumpPath(resolveDumpDir(dumpSpec), verifyLinkId(verifiedBuf),
                                    scan.firstOffset);
    std::ofstream f(path);
    if (!f.is_open()) {
        std::stringstream w;
        w << "data-verification dump: cannot create file '" << path << "'";
        RecordWarning(w);
        return "";
    }

    f << "# sailbandwidth data-verification mismatch dump\n";
    writeIdentity(f, "# ", srcBuf, dstBuf);
    field(f, "# ", "buffer") << (void*)buffer << "\n";
    writeStats(f, "# ", scan, numElements);
    field(f, "# ", "shown below") << scan.listed.size() << " of " << scan.total << " bad values";
    if (scan.total > scan.listed.size()) {
        f << " (set SAILBW_DUMP_SIZE=ALL to list every one)";
    }
    f << "\n";
    writeDumpLegend(f, scan.attributable);

    if (scan.attributable) {
        f << "#             offset        expected          actual             xor     src_content"
          << "    source  transfer\n";
        for (const VerifyMismatch& m : scan.listed) {
            f << std::setw(20) << std::dec << m.offset
              << "      " << hex8(m.expected)
              << "      " << hex8(m.actual)
              << "      " << hex8(m.expected ^ m.actual)
              << "      " << hex8(m.srcValue)
              << "  " << std::setw(8) << srcVerdict(scan, m)
              << "  " << std::setw(8) << transferVerdict(scan, m)
              << "\n";
        }
    } else {
        f << "#             offset        expected          actual             xor\n";
        for (const VerifyMismatch& m : scan.listed) {
            f << std::setw(20) << std::dec << m.offset
              << "      " << hex8(m.expected)
              << "      " << hex8(m.actual)
              << "      " << hex8(m.expected ^ m.actual)
              << "\n";
        }
    }

    f.flush();
    if (!f.good()) {
        std::stringstream w;
        w << "data-verification dump: failed to write file '" << path << "'";
        RecordWarning(w);
        return "";
    }
    return path;
}

}  // namespace

int verifyLinkId(const MemcpyBuffer* buf) {
    int rank = buf->getMPIRank();
    return rank >= 0 ? rank : buf->getBufferIdx();
}

void verifyInjectFaultIfRequested(HGdeviceptr buffer, unsigned long long size, int linkSrcId,
                                  int linkDstId, const std::shared_ptr<NodeHelper>& nodeHelper) {
    if (!faultMatchesLink(linkSrcId, linkDstId)) {
        return;
    }
    // Zeros guarantee a mismatch (the xorshift pattern never yields 0).
    unsigned int anomalies[8] = {0};
    size_t corruptBytes = static_cast<size_t>(std::min<unsigned long long>(sizeof(anomalies), size));
    HG_ASSERT(hgMemcpyAsync(buffer, (HGdeviceptr)anomalies, corruptBytes, HG_STREAM_PER_THREAD));
    HG_ASSERT(nodeHelper->streamSynchronizeWrapper(HG_STREAM_PER_THREAD));
}

void verifyReportFailure(const MemcpyBuffer* srcBuf, const MemcpyBuffer* dstBuf,
                         const MemcpyBuffer* verifiedBuf, const unsigned int* hostPattern,
                         const unsigned int* hostInitPattern, unsigned long long numElements,
                         unsigned int numPatternElements) {
    HGdeviceptr buffer = verifiedBuf->getBuffer();
    // Written on every failure, since that is when the detail is wanted; SAILBW_DUMP_PATH only
    // selects a directory other than the current one.
    const char* dumpSpec = getenv("SAILBW_DUMP_PATH");
    if (dumpSpec == nullptr) {
        dumpSpec = ".";
    }
    const unsigned long long listLimit = parseDumpSize();
    VerifyScan scan = scanVerifiedBuffer(srcBuf, verifiedBuf, buffer, hostPattern, hostInitPattern,
                                         numElements, numPatternElements, listLimit);

    std::stringstream r;
    r << "Data verification FAILED\n";
    writeIdentity(r, "  ", srcBuf, dstBuf);
    if (scan.total == 0) {
        field(r, "  ", "note") << "the check found a mismatch but reading the buffer back showed none"
                                  " (may be transient)\n";
        RecordError(r);
        return;
    }

    HGdeviceptr firstAddr = (HGdeviceptr)((char*)buffer + scan.firstOffset * sizeof(unsigned int));
    field(r, "  ", "first bad address") << (void*)firstAddr << "\n";
    field(r, "  ", "first bad offset") << scan.firstOffset << " (word) / "
                                       << scan.firstOffset * sizeof(unsigned int) << " (byte)\n";
    writeStats(r, "  ", scan, numElements);
    if (faultMatchesLink(verifyLinkId(srcBuf), verifyLinkId(dstBuf))) {
        field(r, "  ", "note") << "values corrupted by SAILBW_VERIFY (simulated fault)\n";
    }

    std::string path = writeDumpFile(dumpSpec, srcBuf, dstBuf, verifiedBuf, buffer, scan,
                                     numElements);
    if (!path.empty()) {
#ifdef MULTINODE
        field(r, "  ", "dumped file") << localHostname << ":" << path
                                      << " (on the reporting rank's node)\n";
#else
        field(r, "  ", "dumped file") << path << "\n";
#endif
    }
    RecordError(r);
}
