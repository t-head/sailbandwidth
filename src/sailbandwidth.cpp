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

#include <argparse/argparse.hpp>
#include <hgml.h>
#include <iostream>
#include <unistd.h>

#ifdef MULTINODE
#include <mpi.h>
#endif

#include "json_output.h"
#include "kernels.cuh"
#include "output.h"
#include "testcase.h"
#include "version.h"

int sm;
int deviceCount;
int nBlocks = -1;
int nThreads = -1;
unsigned int averageLoopCount;
unsigned long long bufferSize;
unsigned long long loopCount;
bool verbose;
bool shouldOutput = true;
bool disableAffinity;
bool skipVerification;
bool useMean;
bool perfFormatter;
bool isSingleDie;
bool useNormalCopy;
bool enableVpipeIdReadOptimize;
bool enableSplitSizeReadOptimize;
bool useSingleLinkForIcnRead;
int linkId = 0;
int vpipeIdForDie0 = -1;
int vpipeIdForDie1 = -1;
bool testLocalCopy;
bool displayUR;
bool disableVm;
long long targetNumPairs = -1;

Verbosity VERBOSE(verbose);
Verbosity OUTPUT(shouldOutput);

#ifdef MULTINODE
// Device ordinal of the PPU owned by the process
int localRank;
// Process rank within one OS
int localDevice;
int worldRank;
int worldSize;
#endif
char localHostname[STRING_LENGTH];
std::string currentTestcaseName;
bool jsonOutput;
Output *output;

// Define testcases here
std::vector<Testcase*> createTestcases() {
    return {
        new HostToDeviceCE(),
        new DeviceToHostCE(),
        new HostToDeviceBidirCE(),
        new DeviceToHostBidirCE(),
        new DeviceToDeviceReadCE(),
        new DeviceToDeviceWriteCE(),
        new DeviceToDeviceBidirReadCE(),
        new DeviceToDeviceBidirWriteCE(),
        new AllToHostCE(),
        new AllToHostBidirCE(),
        new HostToAllCE(),
        new HostToAllBidirCE(),
        new AllToOneWriteCE(),
        new AllToOneReadCE(),
        new OneToAllWriteCE(),
        new OneToAllReadCE(),
        new HostToDeviceSM(),
        new DeviceToHostSM(),
        new HostToDeviceBidirSM(),
        new DeviceToHostBidirSM(),
        new DeviceToDeviceReadSM(),
        new DeviceToDeviceWriteSM(),
        new DeviceToDeviceBidirReadSM(),
        new DeviceToDeviceBidirWriteSM(),
        new AllToHostSM(),
        new AllToHostBidirSM(),
        new HostToAllSM(),
        new HostToAllBidirSM(),
        new AllToOneWriteSM(),
        new AllToOneReadSM(),
        new OneToAllWriteSM(),
        new OneToAllReadSM(),
        new HostDeviceLatencySM(),
        new DeviceToDeviceLatencySM(),
        new DeviceLocalCopy(),
#ifdef MULTINODE
        new MultinodeDeviceToDeviceReadCE(),
        new MultinodeDeviceToDeviceWriteCE(),
        new MultinodeDeviceToDeviceBidirReadCE(),
        new MultinodeDeviceToDeviceBidirWriteCE(),
        new MultinodeDeviceToDeviceReadSM(),
        new MultinodeDeviceToDeviceWriteSM(),
        new MultinodeDeviceToDeviceBidirReadSM(),
        new MultinodeDeviceToDeviceBidirWriteSM(),
        new MultinodeAllToOneWriteSM(),
        new MultinodeAllFromOneReadSM(),
        // Currently not supported on PPU
        // new MultinodeBroadcastOneToAllSM(),
        // new MultinodeBroadcastAllToAllSM(),
        // new MultinodeBisectWriteCE(),
#endif
    };
}

Testcase* findTestcase(std::vector<Testcase*> &testcases, std::string id) {
    // Check if testcase ID is index
    char* p;
    long index = strtol(id.c_str(), &p, 10);
    if (*p) {
        // Conversion failed so key is ID
        auto it = find_if(testcases.begin(), testcases.end(), [&id](Testcase* test) {return test->testKey() == id;});
        if (it != testcases.end()) {
            return testcases.at(std::distance(testcases.begin(), it));
        } else {
            throw "Testcase " + id + " not found!";
        }
    } else {
        // ID is index
        if (index < 0 || index >= static_cast<long>(testcases.size())) throw "Testcase index " + id + " out of bound!";
        return testcases.at(index);
    }
}

std::vector<std::string> expandTestcases(std::vector<Testcase*> &testcases, std::vector<std::string> prefixes) {
    std::vector<std::string> testcasesToRun;
    for (auto testcase : testcases) {
         auto it = find_if(prefixes.begin(), prefixes.end(), [&testcase](std::string prefix) {return testcase->testKey().compare(0, prefix.size(), prefix) == 0;});
            if (it != prefixes.end()) {
                testcasesToRun.push_back(testcase->testKey());
            }
    }
    return testcasesToRun;
}

void runTestcase(std::vector<Testcase*> &testcases, const std::string &testcaseID) {
    Testcase* test{nullptr};
    try {
        test = findTestcase(testcases, testcaseID);
    } catch (std::string &s) {
        output->addTestcase(testcaseID, "ERROR", s);
        return;
    }

    try {
        if (!test->filter()) {
            output->addTestcase(test->testKey(), SBW_WAIVED);
            return;
        }

        output->addTestcase(test->testKey(), SBW_RUNNING);
        currentTestcaseName = test->testKey();

        // Run the testcase
        if (test->testKey() == "host_device_latency_sm" || test->testKey() == "device_to_device_latency_sm") {
            // use fixed-size buffer for latency tests
            test->run(2 * _MiB, loopCount);
        } else {
            test->run(bufferSize * _MiB, loopCount);
        }
    } catch (std::string &s) {
        output->setTestcaseStatusAndAddIfNeeded(test->testKey(), SBW_ERROR_STATUS, s);
    }
}

int main(int argc, char **argv) {
    std::vector<Testcase*> testcases = createTestcases();
    std::vector<std::string> testcasesToRun;
    std::vector<std::string> testcasePrefixes;
    long long bufferSizeArg = defaultBufferSize;
    int averageLoopCountArg = defaultAverageLoopCount;
    long long loopCountArg = defaultLoopCount;
    output = new Output();

#ifdef _WIN32
    strncpy(localHostname, getenv("COMPUTERNAME"), STRING_LENGTH - 1);
    const char* computername = getenv("COMPUTERNAME");
    if (computername && computername[0] != '\0') {
        snprintf(localHostname, STRING_LENGTH, "%s", computername);
    } else {
        snprintf(localHostname, STRING_LENGTH, "%s", "unknown");
    }
#else
    ASSERT(0 == gethostname(localHostname, STRING_LENGTH - 1));

#endif
#ifdef MULTINODE
    // Set up MPI
    MPI_Init(NULL, NULL);
    MPI_Comm_size(MPI_COMM_WORLD, &worldSize);
    MPI_Comm_rank(MPI_COMM_WORLD, &worldRank);

    // Avoid excessive output by limit output to rank 0
    shouldOutput = (worldRank == 0);
#endif

    // Args parsing
    argparse::ArgumentParser program("sailbandwidth", SAILBANDWIDTH_VERSION, argparse::default_arguments::none);
    program.add_argument("-h", "--help").help("Produce help message").flag();
    program.add_argument("-b", "--bufferSize")
        .help("Memcpy buffer size in MiB [default: " + std::to_string(defaultBufferSize) + "]")
        .store_into(bufferSizeArg);
    program.add_argument("-l", "--list").help("List available testcases").flag();
    program.add_argument("-t", "--testcase").help("Testcase(s) to run (by name or index)")
        .nargs(argparse::nargs_pattern::at_least_one).append().store_into(testcasesToRun);
    program.add_argument("-p", "--testcasePrefixes").help("Testcase(s) to run (by prefix)")
        .nargs(argparse::nargs_pattern::at_least_one).append().store_into(testcasePrefixes);
    program.add_argument("-v", "--verbose").help("Verbose output").store_into(verbose);
    program.add_argument("-s", "--skipVerification").help("Skips data verification after copy").store_into(skipVerification);
    program.add_argument("-d", "--disableAffinity").help("Disable automatic CPU affinity control").store_into(disableAffinity);
    program.add_argument("-i", "--testSamples")
        .help("Iterations of the benchmark [default: " + std::to_string(defaultAverageLoopCount) + "]")
        .store_into(averageLoopCountArg);
    program.add_argument("-m", "--useMean").help("Use mean instead of median for results").store_into(useMean);
    program.add_argument("--nBlocks").help("nBlocks for sm bulk copy test [default: -1]").store_into(nBlocks);
    program.add_argument("--nThreads").help("nThreads for sm bulk copy test [default: -1]").store_into(nThreads);
    program.add_argument("--displayUR").help("Show utilization ratio matrices in D2D Tests").store_into(displayUR);
    program.add_argument("--enableSplitSizeReadOptimize").help("Enable special-case read optimization")
        .store_into(enableSplitSizeReadOptimize);
    program.add_argument("-P", "--targetNumPairs").help("Target pairs for multinode tests [default: -1]")
        .metavar("N").store_into(targetNumPairs);

    program.add_argument("--loopCount").help("Iterations of memcpy to be performed within a test sample")
        .store_into(loopCountArg).hidden();
    program.add_argument("--perfFormatter").help("Use perf formatter prefix (&&&& PERF) in output")
        .store_into(perfFormatter).hidden();
    program.add_argument("--useNormalCopy").help("Use normal copy for sm test").store_into(useNormalCopy).hidden();
    program.add_argument("--disableVm").help("Disable virtual mode").store_into(disableVm).hidden();
    program.add_argument("--isSingleDie").help("Use single die for ce test").store_into(isSingleDie).hidden();
    program.add_argument("--testLocalCopy").help("Test local copy in D2D Tests").store_into(testLocalCopy).hidden();
    // Not store_into(jsonOutput): setting it mid-parse would silence the text error report.
    program.add_argument("-j", "--json").help("Print output in json format instead of plain text.")
        .flag().hidden();
    // ICN read vpipe id controls, only effective when built with --use_vpipeid
    program.add_argument("--enableVpipeIdReadOptimize").help("Enable vpipe id read optimization")
        .store_into(enableVpipeIdReadOptimize).hidden();
    program.add_argument("--useSingleLinkForIcnRead").help("Use specified vpipe ids for ICN read DMA")
        .store_into(useSingleLinkForIcnRead).hidden();
    program.add_argument("--linkId").help("Set link number, valid range is 0-PPU_ICN_LINK-1")
        .store_into(linkId).hidden();
    program.add_argument("--vpipeIdForDie0").help("Vpipe id for die0, valid range is 0-PPU_ICN_LINK_PER_DIE-1")
        .store_into(vpipeIdForDie0).hidden();
    program.add_argument("--vpipeIdForDie1").help("Vpipe id for die1, valid range is 0-PPU_ICN_LINK_PER_DIE-1")
        .store_into(vpipeIdForDie1).hidden();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception &err) {
        output->addVersionInfo();

        std::stringstream errmsg;
        errmsg << "ERROR: Invalid Arguments: " << err.what() << std::endl;
        for (int i = 0; i < argc; i++) {
            errmsg << argv[i] << " ";
        }
        std::vector<std::string> messageParts;
        std::stringstream buf;
        buf << program;
        messageParts.emplace_back(errmsg.str());
        messageParts.emplace_back(buf.str());
        output->recordError(messageParts);
        return 1;
    }

    jsonOutput = program.get<bool>("--json");
    if (jsonOutput) {
        delete output;
        output = new JsonOutput(shouldOutput);
    }

    output->addVersionInfo();

    if (program.get<bool>("--help")) {
        OUTPUT << program.help().str() << "\n";
        return 0;
    }

    if (program.get<bool>("--list")) {
        output->listTestcases(testcases);
        return 0;
    }

    if (testcasePrefixes.size() != 0 && testcasesToRun.size() != 0) {
        output->recordError("You cannot specify both testcase and testcasePrefixes options at the same time");
        return 1;
    }

    if (bufferSizeArg <= 0 || averageLoopCountArg <= 0 || loopCountArg <= 0) {
        output->recordError("bufferSize, testSamples and loopCount must be greater than 0");
        return 1;
    }
    if (nBlocks != -1 && nBlocks <= 0) {
        output->recordError("nBlocks must be -1 (auto) or greater than 0");
        return 1;
    }
    if (nThreads != -1 && nThreads <= 0) {
        output->recordError("nThreads must be -1 (auto) or greater than 0");
        return 1;
    }

    bufferSize = static_cast<unsigned long long>(bufferSizeArg);
    averageLoopCount = static_cast<unsigned int>(averageLoopCountArg);
    loopCount = static_cast<unsigned long long>(loopCountArg);

    if (targetNumPairs < -1) {
        std::stringstream errmsg;
        errmsg << "ERROR: Invalid targetNumPairs value: " << targetNumPairs
               << ". Must be -1 (all pairs), 0 (no pairs), or a positive number.";
        output->recordError(errmsg.str());
        return 1;
    }
#ifdef MULTINODE
    if (targetNumPairs > 0) {
        long long maxPairs = static_cast<long long>(worldSize) * (worldSize - 1);
        if (targetNumPairs > maxPairs) {
            std::stringstream errmsg;
            errmsg << "ERROR: targetNumPairs (" << targetNumPairs
                   << ") exceeds maximum possible pairs (" << maxPairs
                   << ") for worldSize " << worldSize
                   << ". Use -1 for all pairs or specify a value <= " << maxPairs << ".";
            output->recordError(errmsg.str());
            return 1;
        }
    }
#endif


    HG_ASSERT(hgInit(0));
    HGML_ASSERT(hgmlInit());
    HG_ASSERT(hgDeviceGetCount(&deviceCount));
    if (bufferSize < defaultBufferSize) {
        output->recordWarning("NOTE: You have chosen a buffer size that is smaller than the default buffer size. It is suggested to use the default buffer size (512MB) to achieve maximal peak bandwidth.");
    }

    int hggcVersion;
    hggcRuntimeGetVersion(&hggcVersion);

    HG_ASSERT(hgDriverGetVersion(&hggcVersion));

    char driverVersion[HGML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE];
    HGML_ASSERT(hgmlSystemGetDriverVersion(driverVersion, HGML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE));

    output->addHggcAndDriverInfo(hggcVersion, driverVersion);

    output->recordDevices(deviceCount);

    // Collect requested vpipe-id options and warn if build-time support is disabled.
    std::vector<std::string> requestedVpipeIdOptions;
    if (enableVpipeIdReadOptimize) {
        requestedVpipeIdOptions.emplace_back("--enableVpipeIdReadOptimize");
    }
    if (useSingleLinkForIcnRead) {
        requestedVpipeIdOptions.emplace_back("--useSingleLinkForIcnRead");
    }
    if (program.is_used("--linkId")) {
        requestedVpipeIdOptions.emplace_back("--linkId " + std::to_string(linkId));
    }
    if (program.is_used("--vpipeIdForDie0")) {
        requestedVpipeIdOptions.emplace_back("--vpipeIdForDie0 " + std::to_string(vpipeIdForDie0));
    }
    if (program.is_used("--vpipeIdForDie1")) {
        requestedVpipeIdOptions.emplace_back("--vpipeIdForDie1 " + std::to_string(vpipeIdForDie1));
    }

    if (!ppuVpipeIdEnable && !requestedVpipeIdOptions.empty()) {
        std::stringstream warning;
        warning << "Warning: sailbandwidth was built without --use_vpipeid; ignoring the following command(s):";
        for (const auto& option : requestedVpipeIdOptions) {
            warning << " " << option;
        }
        warning << ".\n";
        output->recordWarning(warning.str());
        enableVpipeIdReadOptimize = false;
        useSingleLinkForIcnRead = false;
        linkId = 0;
        vpipeIdForDie0 = -1;
        vpipeIdForDie1 = -1;
    }

    sm = getDeviceCompCap();

    if (testcasePrefixes.size() > 0) {
        testcasesToRun = expandTestcases(testcases, testcasePrefixes);
        if (testcasesToRun.size() == 0) {
            output->recordError("Specified list of testcase prefixes did not match any testcases");
            return 1;
        }
    }

    // This triggers the loading of all kernels on all devices, even with lazy loading enabled.
    // Some tests can create complex dependencies between devices and function loading requires a
    // device synchronization, so loading in the middle of a test can deadlock.
    preloadKernels(deviceCount);

    if (testcasesToRun.size() == 0) {
        // run all testcases
        for (auto testcase : testcases) {
            runTestcase(testcases, testcase->testKey());
        }
    } else {
        for (const auto& testcaseIndex : testcasesToRun) {
            runTestcase(testcases, testcaseIndex);
        }
    }

    output->print();

    for (auto testcase : testcases) {
        delete testcase;
    }

#ifdef MULTINODE
    MPI_Finalize();
#endif

    output->printInfo();
    return 0;
}
