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

#ifndef PERF_PARSER_OUTPUT_H_
#define PERF_PARSER_OUTPUT_H_

#include "common.h"
#include "output.h"

#include <vector>

class PerfParserOutput : public Output {
 public:
    explicit PerfParserOutput(bool _shouldOutput);

    void addTestcase(const std::string &name, const std::string &status, const std::string &msg = "");

    void setTestcaseStatusAndAddIfNeeded(const std::string &name, const std::string &status, const std::string &msg = "");

    void print();

    void recordError(const std::string &error);

    void recordError(const std::vector<std::string> &errorParts);

    void recordErrorCurrentTest(const std::string &errorPart1, const std::string &errorPart2);

    void recordWarning(const std::string &warning);

    void addHggcAndDriverInfo(int hggcDriverApiVersion, const std::string &hgmlDriverVersion, const std::string &sdkVersion);

    void addTestcaseResults(const PeerValueMatrix<double> &matrix, const std::string &description);

    void addVersionInfo();

    void addSdkVersion(const std::string &sdkVersion);

    void printInfo();

    void recordDevices(int deviceCount);

    void listTestcases(const std::vector<Testcase*> &testcases);

    void printHelp(const std::string &help);

 private:
    bool shouldOutput;
    bool testFailed;
    bool printed;
    bool ran;
    std::string currentTestName;
    // Buffer the formatted lines so numeric formatting remains unchanged.
    std::vector<std::string> testResults;
};

#endif  // PERF_PARSER_OUTPUT_H_
