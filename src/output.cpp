/*
 * SPDX-FileCopyrightText: Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "inline_common.h"
#include "output.h"
#include "version.h"

#include <cctype>
#include <math.h>
#include <unistd.h>

#ifdef MULTINODE
#include <mpi.h>
#include <map>
#endif

void Output::addVersionInfo() {
    OUTPUT << "sailbandwidth Version: " << SAILBANDWIDTH_VERSION << std::endl;
    // OUTPUT << "Built from Git version: " << GIT_VERSION << std::endl << std::endl;

#ifdef MULTINODE
    char MPIVersion[MPI_MAX_LIBRARY_VERSION_STRING];
    int MPIVersionLen;
    MPI_Get_library_version(MPIVersion, &MPIVersionLen);

    OUTPUT << "MPI version: " << MPIVersion << std::endl;
#endif
}

void Output::printInfo() {
    OUTPUT << "NOTE: The reported results may not reflect the full capabilities of the platform." << std::endl
           << "Performance can vary with software drivers, hardware clocks, and system topology." << std::endl << std::endl;
}

void Output::addHggcAndDriverInfo(int hggcVersion, const std::string &driverVersion) {
    OUTPUT << "HGGC Runtime Version: " << hggcVersion << std::endl;
    OUTPUT << "HGGC Driver Version: " << hggcVersion << std::endl;
    OUTPUT << "Driver Version: " << driverVersion << std::endl << std::endl;
}

void Output::recordError(const std::string &error) {
    std::cerr << error << std::endl;
}

void Output::recordError(const std::vector<std::string> &errorParts) {
    bool first = true;
    for (auto &part : errorParts) {
        if (first) {
            OUTPUT << part << ":\n\n";
            first = false;
        } else {
            OUTPUT << part << std::endl;
        }
    }
}

void Output::listTestcases(const std::vector<Testcase*> &testcases) {
    size_t numTestcases = testcases.size();
    OUTPUT << "Index, Name:\n\tDescription\n";
    OUTPUT << "=======================\n";
    for (unsigned int i = 0; i < numTestcases; i++) {
        OUTPUT << i << ", " << testcases.at(i)->testKey() << ":\n" << testcases.at(i)->testDesc() << "\n\n";
    }
}

// "domain:bus:device" PCI triple, e.g. 00000001:c9:00.
static std::string getDevicePciTriple(int deviceOrdinal) {
    std::stringstream sstream;
    HGdevice dev;
    int busId, deviceId, domainId;

    HG_ASSERT(hgDeviceGet(&dev, deviceOrdinal));
    HG_ASSERT(hgDeviceGetAttribute(&domainId, HG_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID, dev));
    HG_ASSERT(hgDeviceGetAttribute(&busId, HG_DEVICE_ATTRIBUTE_PCI_BUS_ID, dev));
    HG_ASSERT(hgDeviceGetAttribute(&deviceId, HG_DEVICE_ATTRIBUTE_PCI_DEVICE_ID, dev));
    sstream << std::hex << std::setw(8) << std::setfill('0') << domainId << ":" <<
        std::hex << std::setw(2) << std::setfill('0') << busId << ":" <<
        std::hex << std::setw(2) << std::setfill('0') << deviceId <<
        std::dec << std::setfill(' ') << std::setw(0);  // reset formatting
    return sstream.str();
}

std::string getDeviceDisplayInfo(int deviceOrdinal) {
    HGdevice dev;
    char name[STRING_LENGTH];

    HG_ASSERT(hgDeviceGet(&dev, deviceOrdinal));
    HG_ASSERT(hgDeviceGetName(name, STRING_LENGTH, dev));
    return std::string(name) + " (" + getDevicePciTriple(deviceOrdinal) + ")";
}

// Upper-case PCI BDF of a local device ordinal, e.g. "00000001:C9:00.0".
std::string getDeviceBDF(int deviceOrdinal) {
    std::string bdf = getDevicePciTriple(deviceOrdinal);
    for (char& c : bdf) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return bdf + ".0";
}

std::string getLocalHostname() {
    char name[STRING_LENGTH] = {0};
    if (gethostname(name, sizeof(name) - 1) != 0) {
        return "unknown";
    }
    return std::string(name);
}

#ifdef MULTINODE
// Per-rank PPU BDF and hostname, populated by exchangeDeviceInfo().
std::vector<std::string> ppuBDFByRank;
std::vector<std::string> ppuHostByRank;

// Exchange information about all devices in the MPI world. Each process learns
// about PPUs owned by other processes and determines its own local PPU index.
void exchangeDeviceInfo(int deviceCount, std::vector<char> &hostnameExchange,
                        std::vector<char> &deviceNameExchange,
                        std::vector<int> &localDeviceIdExchange) {
    // Exchange hostnames
    MPI_Allgather(localHostname, STRING_LENGTH, MPI_BYTE, hostnameExchange.data(),
                  STRING_LENGTH, MPI_BYTE, MPI_COMM_WORLD);

    // localRank is the number of lower world ranks running on the same host.
    localRank = 0;
    for (int i = 0; i < worldRank; i++) {
        if (strncmp(localHostname, &hostnameExchange[i * STRING_LENGTH], STRING_LENGTH) == 0) {
            localRank++;
        }
    }

    std::vector<int> deviceCountExchange(worldSize);
    MPI_Allgather(&deviceCount, 1, MPI_INT, deviceCountExchange.data(), 1, MPI_INT, MPI_COMM_WORLD);

    localDevice = localRank % deviceCount;

    // It's not recommended to run more ranks per node than PPU count, but we want to make sure we handle it gracefully
    std::map<std::string, int> ppuCounts;
    for (int i = 0; i < worldSize; i++) {
        std::string host(&hostnameExchange[i * STRING_LENGTH]);
        ppuCounts[host]++;
        if (ppuCounts[host] == deviceCountExchange[i] + 1) {
            // Unconditionally emitting a warning, once per node
            std::stringstream warning;
            warning << "Warning: there are more processes than PPUs on " << host << ". Please reduce number of processes to match PPU count.";
            output->recordWarning(warning.str());
        }
    }

    // Exchange device names
    std::string localDeviceName = getDeviceDisplayInfo(localDevice);
    ASSERT(localDeviceName.size() < STRING_LENGTH);
    localDeviceName.resize(STRING_LENGTH);
    MPI_Allgather(localDeviceName.data(), STRING_LENGTH, MPI_BYTE, deviceNameExchange.data(),
                  STRING_LENGTH, MPI_BYTE, MPI_COMM_WORLD);

    // Exchange device ids
    MPI_Allgather(&localDevice, 1, MPI_INT, localDeviceIdExchange.data(), 1, MPI_INT, MPI_COMM_WORLD);

    // Exchange PCI BDF of each rank's PPU
    std::string localBDF = getDeviceBDF(localDevice);
    ASSERT(localBDF.size() < STRING_LENGTH);
    localBDF.resize(STRING_LENGTH);
    std::vector<char> bdfExchange(worldSize * STRING_LENGTH, 0);
    MPI_Allgather(localBDF.data(), STRING_LENGTH, MPI_BYTE, bdfExchange.data(),
                  STRING_LENGTH, MPI_BYTE, MPI_COMM_WORLD);

    ppuBDFByRank.assign(worldSize, "");
    ppuHostByRank.assign(worldSize, "");
    for (int i = 0; i < worldSize; i++) {
        ppuBDFByRank[i] = std::string(&bdfExchange[i * STRING_LENGTH]);
        ppuHostByRank[i] = std::string(&hostnameExchange[i * STRING_LENGTH]);
    }
}

static void printPPUsMultinode(int deviceCount) {
    std::vector<char> hostnameExchange(worldSize * STRING_LENGTH, 0);
    std::vector<char> deviceNameExchange(worldSize * STRING_LENGTH, 0);
    std::vector<int> localDeviceIdExchange(worldSize, -1);
    exchangeDeviceInfo(deviceCount, hostnameExchange, deviceNameExchange, localDeviceIdExchange);

    // Print gathered info
    for (int i = 0; i < worldSize; i++) {
        char *deviceName = &deviceNameExchange[i * STRING_LENGTH];
        OUTPUT << "Process " << getPaddedProcessId(i) << " (" << &hostnameExchange[i * STRING_LENGTH] << "): device " << localDeviceIdExchange[i] << ": " << deviceName << std::endl;
    }
    OUTPUT << std::endl;
}
#endif

static void printPPUs() {
    OUTPUT << localHostname << std::endl;
    for (int iDev = 0; iDev < deviceCount; iDev++) {
        OUTPUT << "Device " << iDev << ": " << getDeviceDisplayInfo(iDev) << std::endl;
    }
    OUTPUT << std::endl;
}

void Output::recordDevices(int deviceCount) {
#ifdef MULTINODE
    printPPUsMultinode(deviceCount);
#else
    printPPUs();
#endif
}

void Output::addTestcase(const std::string &name, const std::string &status, const std::string &msg) {
    if (status == SBW_RUNNING) {
        OUTPUT << status << " " << name << ".\n";
    } else {
        OUTPUT << status << ": " << msg << std::endl;
    }
}

void Output::setTestcaseStatusAndAddIfNeeded(const std::string &name, const std::string &status, const std::string &msg) {
    // For plain text output, the name has always been printed already and therefore isn't needed here
    OUTPUT << status << ": " << msg << std::endl;
}

void Output::addTestcaseResults(const PeerValueMatrix<double> &bandwidthValues, const std::string &description) {
    OUTPUT << description << std::endl;
    OUTPUT << std::fixed << std::setprecision(2) << bandwidthValues << std::endl;
}

void Output::displayUtilizationRatioMatrix(const PeerValueMatrix<double> &bandwidthValues,
                                           bool isMultiTest,
                                           const std::string &prefix,
                                           const std::string &suffix) {
    if (displayUR) {
        PeerValueMatrix<double> bandwidthURValues(bandwidthValues.m_rows,
                                                  bandwidthValues.m_columns,
                                                  bandwidthValues.key);
        if (computeUR(bandwidthValues, bandwidthURValues, isMultiTest)) {
            Output::addTestcaseResults(bandwidthURValues, prefix + suffix);
        } else {
            // output->recordWarning("Failed to get singleDieMinPath. The icnlink number is set as 1 by default.");
            WARN("Unable to display Bandwidth Utilization Ratio Matrix.");
            displayUR = false;
        }
    }
}

void Output::print() {
    // NO-OP
}

void Output::recordErrorCurrentTest(const std::string &errorLine1, const std::string &errorLine2) {
    OUTPUT << errorLine1 << std::endl << errorLine2 << std::endl;
}

void Output::recordWarning(const std::string &warning) {
    OUTPUT << warning << std::endl;
}

void RecordError(const std::stringstream &errmsg) {
    output->recordError(errmsg.str());
}

void RecordWarning(const std::stringstream &warnmsg) {
    output->recordWarning(warnmsg.str());
}
