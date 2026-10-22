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

#include "environment.h"
#include "hggc_version_check.h"
#include "json_output.h"
#include "perf_parser_output.h"
#include "kernels.cuh"
#include "output.h"
#include "test_settings.h"
#include "testcase.h"
#include "sailbandwidth_version.h"

TestSettings gSettings;

int sm;
int deviceCount;
bool shouldOutput = true;

Verbosity VERBOSE(gSettings.verbose);
Verbosity OUTPUT(shouldOutput);

// Device ordinal of the PPU owned by the process (multinode only)
int localDevice = 0;
// Process rank within one OS
int localRank = 0;
int worldRank = 0;
int worldSize = 1;
std::string currentTestcaseName;
bool jsonOutput;
Output *output;

std::unique_ptr<Environment> env;

// Define testcases here
std::vector<Testcase*> createNodeTestcases() {
    std::vector<Testcase*> tests = {
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
    };
#ifdef MULTINODE
    // Multinode testcases are only listed when running with more than one process
    if (env->getSize() > 1) {
        tests.insert(tests.end(), {
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
            // new MultinodeBroadcastOneToAllSM(),
            // new MultinodeBroadcastAllToAllSM(),
            // new MultinodeBisectWriteCE(),
        });
    }
#endif
    return tests;
}

// Single-node testcases only run on world rank 0; the other ranks skip them so the
// testcase list and indices stay identical across ranks.
static bool isSingleNodeTestcase(Testcase *test) {
    return test->testKey().rfind("multinode_", 0) != 0;
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

    if (worldRank != 0 && isSingleNodeTestcase(test)) {
        return;
    }

    try {
        auto filterResult = test->filter();
        if (!filterResult) {
            output->addTestcase(test->testKey(), SBW_WAIVED, filterResult.reason);
            return;
        }

        output->addTestcase(test->testKey(), SBW_RUNNING);
        currentTestcaseName = test->testKey();

        // Run the testcase
        if (test->testKey() == "host_device_latency_sm" || test->testKey() == "device_to_device_latency_sm") {
            // use fixed-size buffer for latency tests
            test->run(2 * _MiB, gSettings.loopCount);
        } else {
            test->run(gSettings.bufferSizeMiB * _MiB, gSettings.loopCount);
        }
    } catch (std::string &s) {
        output->setTestcaseStatusAndAddIfNeeded(test->testKey(), SBW_ERROR_STATUS, s);
    }
}

// Check device selection and a small allocation before running any testcase.
static bool runtimeSanityCheck() {
#ifdef MULTINODE
    const int startDevice = localDevice;
    const int endDevice = localDevice + 1;
#else
    const int startDevice = 0;
    const int endDevice = deviceCount;
#endif
    hggcError_t result = hggcSuccess;
    int api = 0;
    int device = startDevice;
    for (; device < endDevice; device++) {
        api = 0;
        result = hggcSetDevice(device);
        if (result != hggcSuccess) break;

        void* testPtr = nullptr;
        api = 1;
        result = hggcMalloc(&testPtr, 1024);
        if (result != hggcSuccess) break;

        api = 2;
        result = hggcFree(testPtr);
        if (result != hggcSuccess) break;
    }

    const int local[] = {static_cast<int>(result), api, device};
    std::vector<int> gathered(local, local + 3);
#ifdef MULTINODE
    // Gather failures before reporting: nonzero ranks do not print JSON or perf output.
    gathered.resize(static_cast<size_t>(worldSize) * 3);
    ASSERT(MPI_Allgather(local, 3, MPI_INT, gathered.data(), 3, MPI_INT, MPI_COMM_WORLD) == MPI_SUCCESS);
#endif
    const char* apiNames[] = {"hggcSetDevice", "hggcMalloc", "hggcFree"};
    std::ostringstream errors;
    for (size_t rank = 0; rank < gathered.size() / 3; rank++) {
        const auto status = static_cast<hggcError_t>(gathered[rank * 3]);
        if (status == hggcSuccess) continue;
#ifdef MULTINODE
        const std::string& hostname = ppuHostByRank[rank];
#else
        const std::string hostname = env->getHostname();
#endif
        errors << "HGGC runtime sanity check failed on " << hostname
               << ", rank = " << rank << ", device = " << gathered[rank * 3 + 2]
               << ": " << apiNames[gathered[rank * 3 + 1]] << " returned "
               << hggcGetErrorString(status) << " (" << static_cast<int>(status) << ")\n";
    }
    if (worldRank == 0 && !errors.str().empty()) output->recordError(errors.str());
    return errors.str().empty();
}

int main(int argc, char **argv) {
    output = new Output();

    env = Environment::create(argc, argv);
    env->initialize(argc, argv);
    // Finalize the environment on every return path, including early exits
    struct EnvFinalizer {
        ~EnvFinalizer() {
            env->finalize();
        }
    } envFinalizer;

#ifdef MULTINODE
    worldSize = env->getSize();
    worldRank = env->getRank();
    localRank = env->getLocalRank();
    // Avoid excessive output by limit output to rank 0
    shouldOutput = (worldRank == 0);
#endif

    bool legacyJson = false;
    std::string requestedFormat = "text";
    // Use argparse for the format pre-scan as well, including combined short options.
    // Missing values and duplicates are reported by the full parser below.
    argparse::ArgumentParser formatParser("sailbandwidth", SAILBANDWIDTH_VERSION, argparse::default_arguments::none);
    formatParser.add_argument("-j", "--json").store_into(legacyJson).append();
    formatParser.add_argument("-F", "--format").nargs(argparse::nargs_pattern::optional)
        .store_into(requestedFormat).append();
    formatParser.add_argument("-h", "-l", "-v", "-s", "-d", "-m").flag().append();
    formatParser.add_argument("-b", "-i", "-P").nargs(argparse::nargs_pattern::optional).append();
    formatParser.add_argument("-t", "-p").nargs(argparse::nargs_pattern::any).append();
    std::vector<std::string> formatArgs(argv, argv + argc);
    formatArgs.erase(std::find(formatArgs.begin() + 1, formatArgs.end(), "--"), formatArgs.end());
    formatParser.parse_known_args(formatArgs);
    std::string initialFormat = requestedFormat;
    if (initialFormat == "text" && legacyJson) initialFormat = "json";
    auto setOutputFormat = [](const std::string &format) {
        delete output;
        jsonOutput = format == "json" || format == "perf";
        if (format == "json") {
            output = new JsonOutput(shouldOutput);
        } else if (format == "perf") {
            output = new PerfParserOutput(shouldOutput);
        } else {
            output = new Output();
        }
    };
    setOutputFormat(initialFormat);
    std::vector<Testcase*> testcases = createNodeTestcases();
    std::vector<std::string> testcasesToRun;
    std::vector<std::string> testcasePrefixes;

    long long bufferSizeArg = kDefaultBufferSizeMiB;
    int averageLoopCountArg = kDefaultAverageLoopCount;
    long long loopCountArg = kDefaultLoopCount;

    // Args parsing
    argparse::ArgumentParser program("sailbandwidth", SAILBANDWIDTH_VERSION, argparse::default_arguments::none);
    program.add_argument("--version").help("Print version and Git revision").flag();
    program.add_argument("-F", "--format").help("Output format: text, json or perf").store_into(gSettings.outputFormat);
    program.add_argument("-h", "--help").help("Produce help message").flag();
    program.add_argument("-b", "--bufferSize")
        .help("Memcpy buffer size in MiB [default: " + std::to_string(kDefaultBufferSizeMiB) + "]")
        .store_into(bufferSizeArg);
    program.add_argument("-l", "--list").help("List available testcases").flag();
    program.add_argument("-t", "--testcase").help("Testcase(s) to run (by name or index)")
        .nargs(argparse::nargs_pattern::at_least_one).append().store_into(testcasesToRun);
    program.add_argument("-p", "--testcasePrefixes").help("Testcase(s) to run (by prefix)")
        .nargs(argparse::nargs_pattern::at_least_one).append().store_into(testcasePrefixes);
    program.add_argument("-v", "--verbose").help("Verbose output").store_into(gSettings.verbose);
    program.add_argument("-s", "--skipVerification").help("Skips data verification after copy")
        .store_into(gSettings.skipVerification);
    program.add_argument("-d", "--disableAffinity").help("Disable automatic CPU affinity control")
        .store_into(gSettings.disableAffinity);
    program.add_argument("-i", "--testSamples")
        .help("Iterations of the benchmark [default: " + std::to_string(kDefaultAverageLoopCount) + "]")
        .store_into(averageLoopCountArg);
    program.add_argument("-m", "--useMean").help("Use mean instead of median for results").store_into(gSettings.useMean);
    program.add_argument("--nBlocks").help("nBlocks for sm bulk copy test [default: -1]").store_into(gSettings.nBlocks);
    program.add_argument("--nThreads").help("nThreads for sm bulk copy test [default: -1]").store_into(gSettings.nThreads);
    program.add_argument("--displayUR").help("Show utilization ratio matrices in D2D Tests").store_into(gSettings.displayUR);
    program.add_argument("--enableSplitSizeReadOptimize").help("Enable special-case read optimization")
        .store_into(gSettings.enableSplitSizeReadOptimize);
    program.add_argument("-P", "--targetNumPairs").help("Target pairs for multinode tests [default: -1]")
        .metavar("N").store_into(gSettings.targetNumPairs);

    program.add_argument("--loopCount").help("Iterations of memcpy to be performed within a test sample")
        .store_into(loopCountArg).hidden();
    program.add_argument("--perfFormatter").help("Use perf formatter prefix (&&&& PERF) in output")
        .store_into(gSettings.perfFormatter).hidden();
    program.add_argument("--useNormalCopy").help("Use normal copy for sm test").store_into(gSettings.useNormalCopy).hidden();
    program.add_argument("--disableVm").help("Disable virtual mode").store_into(gSettings.disableVm).hidden();
    program.add_argument("--isSingleDie").help("Use single die for ce test").store_into(gSettings.isSingleDie).hidden();
    program.add_argument("--testLocalCopy").help("Test local copy in D2D Tests").store_into(gSettings.testLocalCopy).hidden();
    // Not store_into(jsonOutput): setting it mid-parse would silence the text error report.
    program.add_argument("-j", "--json").help("Print output in json format instead of plain text.")
        .flag().hidden();
    // ICN read vpipe id controls, only effective when built with --use_vpipeid
    program.add_argument("--enableVpipeIdReadOptimize").help("Enable vpipe id read optimization")
        .store_into(gSettings.enableVpipeIdReadOptimize).hidden();
    program.add_argument("--useSingleLinkForIcnRead").help("Use specified vpipe ids for ICN read DMA")
        .store_into(gSettings.useSingleLinkForIcnRead).hidden();
    program.add_argument("--linkId").help("Set link number, valid range is 0-PPU_ICN_LINK-1")
        .store_into(gSettings.linkId).hidden();
    program.add_argument("--vpipeIdForDie0").help("Vpipe id for die0, valid range is 0-PPU_ICN_LINK_PER_DIE-1")
        .store_into(gSettings.vpipeIdForDie0).hidden();
    program.add_argument("--vpipeIdForDie1").help("Vpipe id for die1, valid range is 0-PPU_ICN_LINK_PER_DIE-1")
        .store_into(gSettings.vpipeIdForDie1).hidden();

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

    // The full parse is authoritative for all successful commands.
    std::string parsedFormat = gSettings.outputFormat;
    if (parsedFormat == "text" && program.get<bool>("--json")) parsedFormat = "json";
    if (parsedFormat != initialFormat) setOutputFormat(parsedFormat);
    output->addVersionInfo();
    if (gSettings.outputFormat != "text" && gSettings.outputFormat != "json" && gSettings.outputFormat != "perf") {
        output->recordError("--format must be text, json or perf");
        return 1;
    }
    if (program.get<bool>("--json")) {
        if (gSettings.outputFormat != "text") {
            output->recordError("Cannot use --json with --format; use --format json instead");
            return 1;
        }
        gSettings.outputFormat = "json";
    }
    if (program.get<bool>("--version")) {
        output->print();
        return 0;
    }

    if (program.get<bool>("--help")) {
        output->printHelp(program.help().str());
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
    if (gSettings.nBlocks != -1 && gSettings.nBlocks <= 0) {
        output->recordError("nBlocks must be -1 (auto) or greater than 0");
        return 1;
    }
    if (gSettings.nThreads != -1 && gSettings.nThreads <= 0) {
        output->recordError("nThreads must be -1 (auto) or greater than 0");
        return 1;
    }

    gSettings.bufferSizeMiB = static_cast<unsigned long long>(bufferSizeArg);
    gSettings.averageLoopCount = static_cast<unsigned int>(averageLoopCountArg);
    gSettings.loopCount = static_cast<unsigned long long>(loopCountArg);

    if (gSettings.targetNumPairs < -1) {
        std::stringstream errmsg;
        errmsg << "ERROR: Invalid targetNumPairs value: " << gSettings.targetNumPairs
               << ". Must be -1 (all pairs), 0 (no pairs), or a positive number.";
        output->recordError(errmsg.str());
        return 1;
    }
#ifdef MULTINODE
    if (gSettings.targetNumPairs > 0) {
        long long maxPairs = static_cast<long long>(worldSize) * (worldSize - 1);
        if (gSettings.targetNumPairs > maxPairs) {
            std::stringstream errmsg;
            errmsg << "ERROR: targetNumPairs (" << gSettings.targetNumPairs
                   << ") exceeds maximum possible pairs (" << maxPairs
                   << ") for worldSize " << worldSize
                   << ". Use -1 for all pairs or specify a value <= " << maxPairs << ".";
            output->recordError(errmsg.str());
            return 1;
        }
    }
#endif

    int hggcRuntimeVersion = 0;
    int hggcDriverApiVersion = 0;
    int versionCheckFailed = !validateHggcRuntimeAndDriver(output, &hggcRuntimeVersion, &hggcDriverApiVersion);
#ifdef MULTINODE
    ASSERT(MPI_Allreduce(MPI_IN_PLACE, &versionCheckFailed, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD) == MPI_SUCCESS);
#endif
    if (versionCheckFailed) return 1;

    HG_ASSERT(hgInit(0));
    HGML_ASSERT(hgmlInit());
    HG_ASSERT(hgDeviceGetCount(&deviceCount));
    if (gSettings.bufferSizeMiB < kDefaultBufferSizeMiB) {
        output->recordWarning("NOTE: You have chosen a buffer size that is smaller than the default buffer size. It is suggested to use the default buffer size (512MB) to achieve maximal peak bandwidth.");
    }

    char driverVersion[HGML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE];
    HGML_ASSERT(hgmlSystemGetDriverVersion(driverVersion, HGML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE));

    output->addHggcAndDriverInfo(hggcRuntimeVersion, hggcDriverApiVersion, driverVersion);

    output->recordDevices(deviceCount);

    if (!runtimeSanityCheck()) return 1;

    // Collect requested vpipe-id options and warn if build-time support is disabled.
    std::vector<std::string> requestedVpipeIdOptions;
    if (gSettings.enableVpipeIdReadOptimize) {
        requestedVpipeIdOptions.emplace_back("--enableVpipeIdReadOptimize");
    }
    if (gSettings.useSingleLinkForIcnRead) {
        requestedVpipeIdOptions.emplace_back("--useSingleLinkForIcnRead");
    }
    if (program.is_used("--linkId")) {
        requestedVpipeIdOptions.emplace_back("--linkId " + std::to_string(gSettings.linkId));
    }
    if (program.is_used("--vpipeIdForDie0")) {
        requestedVpipeIdOptions.emplace_back("--vpipeIdForDie0 " + std::to_string(gSettings.vpipeIdForDie0));
    }
    if (program.is_used("--vpipeIdForDie1")) {
        requestedVpipeIdOptions.emplace_back("--vpipeIdForDie1 " + std::to_string(gSettings.vpipeIdForDie1));
    }

    if (!ppuVpipeIdEnable && !requestedVpipeIdOptions.empty()) {
        std::stringstream warning;
        warning << "Warning: sailbandwidth was built without --use_vpipeid; ignoring the following command(s):";
        for (const auto& option : requestedVpipeIdOptions) {
            warning << " " << option;
        }
        warning << ".\n";
        output->recordWarning(warning.str());
        gSettings.enableVpipeIdReadOptimize = false;
        gSettings.useSingleLinkForIcnRead = false;
        gSettings.linkId = 0;
        gSettings.vpipeIdForDie0 = -1;
        gSettings.vpipeIdForDie1 = -1;
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

    // Emit a NOTE explaining that rank pairs are shuffled with or without -P, while final matrices remain ordered by rank.
#ifdef MULTINODE
    if (gSettings.verbose && gSettings.targetNumPairs != 0 && worldSize > 1) {
        bool hasRankPairTestcase = testcasesToRun.empty();
        for (const auto& testcaseID : testcasesToRun) {
            Testcase* test;
            try {
                test = findTestcase(testcases, testcaseID);
            } catch (const std::string&) {
                continue;
            }
            const auto key = test->testKey();
            if (key.find("multinode_device_to_device_memcpy_") == 0 ||
                key.find("multinode_device_to_device_bidirectional_memcpy_") == 0) {
                hasRankPairTestcase = true;
                break;
            }
        }
        if (hasRankPairTestcase) {
            OUTPUT << "NOTE: Multinode D2D rank pairs are executed in a shuffled order, "
                      "final result matrices remain ordered by rank."
                   << std::endl << std::endl;
        }
    }
#endif

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

    output->printInfo();
    return 0;
}
