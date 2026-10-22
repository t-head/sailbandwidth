/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "perf_parser_output.h"
#include "inline_common.h"
#include "sailbandwidth_version.h"

#include <iostream>
#include <sstream>

PerfParserOutput::PerfParserOutput(bool _shouldOutput) {
    shouldOutput = _shouldOutput;
    testFailed = false;
    printed = false;
    ran = false;
}

void PerfParserOutput::addTestcase(const std::string &name, const std::string &status, const std::string &msg) {
    currentTestName = name;
    ran = true;
    if (status == SBW_ERROR_STATUS || status == "ERROR" || status == SBW_NOT_FOUND) testFailed = true;
    if (shouldOutput && status != SBW_RUNNING) std::cout << status << " " << name << ": " << msg << "\n";
}

void PerfParserOutput::setTestcaseStatusAndAddIfNeeded(const std::string &name, const std::string &status, const std::string &msg) {
    addTestcase(name, status, msg);
}

void PerfParserOutput::addTestcaseResults(const PeerValueMatrix<double> &matrix, const std::string &description) {
    const std::string &resultName = matrix.key.empty() ? currentTestName : matrix.key;

    // Iterate through each value in the matrix.
    for (int row = 0; row < matrix.m_rows; row++) {
        for (int col = 0; col < matrix.m_columns; col++) {
            std::optional<double> val = matrix.value(row, col);

            if (!val.has_value()) {
                continue;
            }

            std::ostringstream line;
            line << "&&&& PERF sailbandwidth/" << resultName << "/PPU" << row << "/PPU" << col
                 << " " << val.value() << getUnitString(matrix.uType);
            testResults.push_back(line.str());
        }
    }
}

void PerfParserOutput::print() {
    if (!shouldOutput || printed) return;
    printed = true;

    for (const auto& result : testResults) {
        std::cout << result << "\n";
    }

    if (ran || testFailed) {
        if (!testFailed) {
            std::cout << "&&&& sailbandwidth test PASSED\n";
        } else {
            std::cout << "&&&& sailbandwidth test FAILED\n";
        }
    }
    // API error handlers may call MPI_Abort immediately after recordError().
    std::cout.flush();
}

void PerfParserOutput::addVersionInfo() {
    if (!shouldOutput) return;
    std::cout << "sailbandwidth Version: " << SAILBANDWIDTH_VERSION << "\n";
    std::cout << "Built from Git version: " << GIT_VERSION << "\n";
    std::cout << "HGGC Build Runtime API Version: " << HGGCRT_VERSION << "\n";
    std::cout << "HGGC Build Driver API Version: " << HGGC_VERSION << "\n";
}

void PerfParserOutput::addHggcAndDriverInfo(int hggcRuntimeVersion, int hggcDriverApiVersion,
                                            const std::string &hgmlDriverVersion) {
    if (!shouldOutput) return;
    std::cout << "HGGC Runtime Version: " << hggcRuntimeVersion << "\n";
    std::cout << "HGGC Driver API Version: " << hggcDriverApiVersion << "\n";
    std::cout << "Driver Version: " << hgmlDriverVersion << "\n";
}

void PerfParserOutput::recordDevices(int deviceCount) {
#ifdef MULTINODE
    // Device exchange populates topology information needed by existing tests.
    std::vector<char> hostnameExchange(worldSize * STRING_LENGTH), deviceNameExchange(worldSize * STRING_LENGTH);
    std::vector<int> localDeviceIdExchange(worldSize);
    exchangeDeviceInfo(deviceCount, hostnameExchange, deviceNameExchange, localDeviceIdExchange);
#endif
    if (!shouldOutput) return;
    std::cout << "Devices: " << deviceCount << "\n";
}

void PerfParserOutput::recordError(const std::string &error) {
    testFailed = true;
    if (shouldOutput) {
        std::cout << "ERROR: " << error << "\n";
    } else {
        std::cerr << "ERROR: " << error << std::endl;
        if (!printed) {
            printed = true;
            std::cout << "&&&& sailbandwidth test FAILED" << std::endl;
        }
    }
    print();
}

void PerfParserOutput::recordError(const std::vector<std::string> &errorParts) {
    std::ostringstream message;
    for (const auto& part : errorParts) {
        message << part << "\n";
    }
    recordError(message.str());
}

void PerfParserOutput::recordErrorCurrentTest(const std::string &errorPart1, const std::string &errorPart2) {
    recordError(errorPart1 + " " + errorPart2);
}

void PerfParserOutput::recordWarning(const std::string &warning) {
    if (shouldOutput) std::cout << "WARNING: " << warning << "\n";
}

void PerfParserOutput::printInfo() {
    // No-op for perf parser.
}

void PerfParserOutput::listTestcases(const std::vector<Testcase*> &testcases) {
    if (!shouldOutput) return;
    for (size_t i = 0; i < testcases.size(); i++) {
        std::cout << i << ", " << testcases[i]->testKey() << ": " << testcases[i]->testDesc() << "\n";
    }
}

void PerfParserOutput::printHelp(const std::string &help) {
    if (shouldOutput) std::cout << help << "\n";
}
