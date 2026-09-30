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

#include <cassert>
#include <string>

#include "ppu_common.h"
#include "json_output.h"
#include "version.h"

const std::string SBW_TITLE("sailbandwidth");
const std::string SBW_HOST_NAME("Hostname");
const std::string SBW_HGGC_RUNTIME_VERSION("HGGC Runtime Version");
const std::string SBW_DEVICE_INFO("PPU Device info");
const std::string SBW_DEVICE_LIST("PPU Device list");
const std::string SBW_DRIVER_VERSION("HGGC Driver Version");
const std::string SBW_VERSION("version");
const std::string SBW_ERROR("error");
const std::string SBW_WARNING("warning");
const std::string SBW_TESTCASES("testcases");
const std::string SBW_TESTCASE_NAME("name");
const std::string SBW_TESTCASE_ERROR(SBW_ERROR);
const std::string SBW_STATUS("status");
const std::string SBW_BW_DESCRIPTION("bandwidth_description");
const std::string SBW_BW_MATRIX("bandwidth_matrix");
const std::string SBW_BW_SUM("sum");
const std::string SBW_BW_MAX("max");
const std::string SBW_BW_MIN("min");
const std::string SBW_BW_AVG("average");
const std::string SBW_BUFFER_SIZE("bufferSize");
const std::string SBW_TEST_SAMPLES("testSamples");
const std::string SBW_USE_MEAN("useMean");
const std::string SBW_PASSED("Passed");
const std::string SBW_RUNNING("Running");
const std::string SBW_WAIVED("Waived");
const std::string SBW_NOT_FOUND("Not Found");
const std::string SBW_ERROR_STATUS("Error");

JsonOutput::JsonOutput(bool _shouldOutput) {
    shouldOutput = _shouldOutput;
}

void JsonOutput::addTestcaseResults(const PeerValueMatrix<double> &matrix, const std::string &description) {
    assert(m_root[SBW_TITLE][SBW_TESTCASES].isArray() && m_root[SBW_TITLE][SBW_TESTCASES].size() > 0);

    unsigned int size = m_root[SBW_TITLE][SBW_TESTCASES].size();
    Json::Value &testcase = m_root[SBW_TITLE][SBW_TESTCASES][size-1];

    double maxVal = std::numeric_limits<double>::min();
    double minVal = std::numeric_limits<double>::max();
    double sum = 0;
    int count = 0;

    for (int currentDevice = 0; currentDevice < matrix.m_rows; currentDevice++) {
        Json::Value row;
        for (int peer = 0; peer < matrix.m_columns; peer++) {
            std::optional <double> val = matrix.value(currentDevice, peer);
            if (val) {
                std::stringstream buf;
                buf << val.value();
                row.append(buf.str());
            } else {
                row.append("N/A");
            }
            sum += val.value_or(0.0);
            maxVal = std::max(maxVal, val.value_or(0.0));
            minVal = std::min(minVal, val.value_or(0.0));
            if (val.value_or(0.0) > 0) count++;
        }
        testcase[SBW_BW_MATRIX].append(row);
    }

    testcase[SBW_BW_SUM] = sum;
    testcase[SBW_BW_DESCRIPTION] = description;
    testcase[SBW_STATUS] = SBW_PASSED;

    if (verbose) {
        testcase[SBW_BW_MIN] = minVal;
        testcase[SBW_BW_MAX] = maxVal;
        testcase[SBW_BW_AVG] = sum/count;
    }
}

void JsonOutput::addTestcase(const std::string &name, const std::string &status, const std::string &msg) {
    Json::Value testcase;
    testcase[SBW_TESTCASE_NAME] = name;
    testcase[SBW_STATUS] = status;
    m_root[SBW_TITLE][SBW_TESTCASES].append(testcase);
}

void JsonOutput::recordErrorCurrentTest(const std::string &errorPart1, const std::string &errorPart2) {
    bool testCaseExists = false;
    if (m_root[SBW_TITLE][SBW_TESTCASES].isArray()) {
        Json::Value &testcases = m_root[SBW_TITLE][SBW_TESTCASES];
        unsigned int size = testcases.size();
        if (size > 0) {
            testcases[size-1][SBW_TESTCASE_ERROR] = errorPart1 + " " + errorPart2;
            testCaseExists = true;
        }
    }

    if (!testCaseExists) {
        std::vector<std::string> errors;
        errors.emplace_back(errorPart1);
        errors.emplace_back(errorPart2);
        recordError(errors);
    }
}

void JsonOutput::setTestcaseStatusAndAddIfNeeded(const std::string &name, const std::string &status, const std::string &msg) {
    bool testCaseExists = false;
    if (m_root[SBW_TITLE][SBW_TESTCASES].isArray()) {
        Json::Value &testcases = m_root[SBW_TITLE][SBW_TESTCASES];
        unsigned int size = testcases.size();
        if (size > 0 && testcases[size-1][SBW_TESTCASE_NAME].asString() == name) {
            testcases[size-1][SBW_STATUS] = status;
            testCaseExists = true;
        }
    }

    if (!testCaseExists) {
        addTestcase(name, status);
    }
}

void JsonOutput::recordError(const std::string &error) {
    m_root[SBW_TITLE][SBW_ERROR] = error;
    print();
}

void JsonOutput::recordError(const std::vector<std::string> &errorParts) {
    std::stringstream buf;
    bool first = true;

    for (auto &part : errorParts) {
        if (first) {
            buf << part << ":";
            first = false;
        } else {
            buf << " " << part;
        }
    }
    m_root[SBW_TITLE][SBW_ERROR] = buf.str();
}

void JsonOutput::recordWarning(const std::string &warning) {
    m_root[SBW_TITLE][SBW_WARNING].append(warning);
}

void JsonOutput::addVersionInfo() {
    m_root[SBW_TITLE][SBW_VERSION] = SAILBANDWIDTH_VERSION;
}

void JsonOutput::addHggcAndDriverInfo(int hggcVersion, const std::string &driverVersion) {
    m_root[SBW_TITLE][SBW_HGGC_RUNTIME_VERSION] = hggcVersion;
    m_root[SBW_TITLE][SBW_DRIVER_VERSION] = driverVersion;
}

void JsonOutput::recordDevices(int deviceCount) {
    Json::Value deviceList;

    #ifdef MULTINODE
        std::vector<char> hostnameExchange(worldSize * STRING_LENGTH);
        std::vector<char> deviceNameExchange(worldSize * STRING_LENGTH, 0);
        std::vector<int> localDeviceIdExchange(worldSize, -1);
        exchangeDeviceInfo(deviceCount, hostnameExchange, deviceNameExchange, localDeviceIdExchange);
        for (int i = 0; i < worldSize; i++) {
            char *deviceName = &deviceNameExchange[i * STRING_LENGTH];
            std::stringstream buf;
            buf << "Process " << getPaddedProcessId(i) << " (" << &hostnameExchange[i * STRING_LENGTH] << "): device " << localDeviceIdExchange[i] << ": " << deviceName;
            deviceList.append(buf.str());
        }
    #else
        for (int iDev = 0; iDev < deviceCount; iDev++) {
            std::stringstream buf;
            buf << iDev << ": " << getDeviceDisplayInfo(iDev) << ": (" << localHostname << ")";
            deviceList.append(buf.str());
        }
    #endif
    m_root[SBW_TITLE][SBW_DEVICE_LIST] = deviceList;
}

void JsonOutput::print() {
    if (shouldOutput) {
        std::cout << m_root.toStyledString() << std::endl;
    }
}

void JsonOutput::printInfo() {
    // NO-OP
}
