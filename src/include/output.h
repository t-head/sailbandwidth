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

#ifndef OUTPUT_H_
#define OUTPUT_H_

#include <string>
#include <vector>

#include "testcase.h"

extern const std::string SBW_TITLE;
extern const std::string SBW_HGGC_RUNTIME_VERSION;
extern const std::string SBW_DRIVER_VERSION;
extern const std::string SBW_ERROR;
extern const std::string SBW_WARNING;
extern const std::string SBW_TESTCASES;
extern const std::string SBW_TESTCASE_NAME;
extern const std::string SBW_STATUS;
extern const std::string SBW_BW_DESCRIPTION;
extern const std::string SBW_BW_MATRIX;
extern const std::string SBW_BW_SUM;
extern const std::string SBW_BUFFER_SIZE;
extern const std::string SBW_TEST_SAMPLES;
extern const std::string SBW_USE_MEAN;
extern const std::string SBW_PASSED;
extern const std::string SBW_RUNNING;
extern const std::string SBW_WAIVED;
extern const std::string SBW_NOT_FOUND;
extern const std::string SBW_ERROR_STATUS;

class Output {
 public:
    virtual void addTestcase(const std::string &name, const std::string &status, const std::string &msg = "");

    /*
     * If a test case matching the specified name exists, then update the status. If no testcase with that name exists,
     * then add a new one and set the status.
     *
     * @param name - the name of the test case
     * @param status - the status (PASS, FAIL, WAIVED, NOT FOUND)
     * @param msg - additional details if specified
     */
    virtual void setTestcaseStatusAndAddIfNeeded(const std::string &name, const std::string &status, const std::string &msg = "");

    virtual void print();

    /*
     * Records a global error
     *
     * @param errorParts - each entry in this vector is one line of an error. In JSON output, all lines are combined.
     */
    virtual void recordError(const std::vector<std::string> &errorParts);

    /*
     * Records a global error
     */
    virtual void recordError(const std::string &error);

    /*
     * Records a test error
     *
     * @param errorPart1 - the first part of the error. For plain text output, this is printed on line 1.
     * @param errorPart2 - the second part of the error. For plain text output, this is printed on line 2.
     * NOTE: in JSON output, these are combined on a single line
     */
    virtual void recordErrorCurrentTest(const std::string &errorPart1, const std::string &errorPart2);

    virtual void recordWarning(const std::string &warning);

    virtual void addHggcAndDriverInfo(int hggcVersion, const std::string &driverVersion);

    virtual void addTestcaseResults(const PeerValueMatrix<double> &matrix, const std::string &description);

    virtual void displayUtilizationRatioMatrix(const PeerValueMatrix<double> &bandwidthValues,
                                               bool isMultiTest,
                                               const std::string &prefix,
                                               const std::string &suffix=" bandwidth utilization ratio matrix (%)");

    virtual void addVersionInfo();

    virtual void printInfo();

    virtual void recordDevices(int deviceCount);

    void listTestcases(const std::vector<Testcase*> &testcases);

    virtual ~Output() {};
};

extern Output *output;

std::string getDeviceDisplayInfo(int deviceOrdinal);
std::string getDeviceBDF(int deviceOrdinal);
// Local node hostname; "unknown" if unavailable.
std::string getLocalHostname();

#ifdef MULTINODE
void exchangeDeviceInfo(int deviceCount, std::vector<char> &hostnameExchange,
                        std::vector<char> &deviceNameExchange,
                        std::vector<int> &localDeviceIdExchange);

// Per-rank PPU BDF and hostname, indexed by MPI worldRank.
extern std::vector<std::string> ppuBDFByRank;
extern std::vector<std::string> ppuHostByRank;
#endif

#endif  // OUTPUT_H_
