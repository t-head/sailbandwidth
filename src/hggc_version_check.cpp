/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * Modifications Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
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

#include "hggc_version_check.h"

#include "error_handling.h"
#include "output.h"

#include <hggc_ad.h>
#include <hggc_runtime.h>

#include <sstream>

std::string formatHggcVersionCode(int hggcVersionCode) {
    const int major = hggcVersionCode / 1000;
    const int minor = (hggcVersionCode % 1000) / 10;
    std::ostringstream oss;
    oss << major << '.' << minor;
    return oss.str();
}

std::string getHggcSdkVersion() {
    int driverSdkVersion = 0;
    HG_ASSERT(hgDriverGetSdkVersionAD(&driverSdkVersion, nullptr));
    return std::to_string(driverSdkVersion / 10000) + "." +
           std::to_string((driverSdkVersion / 100) % 100) + "." +
           std::to_string(driverSdkVersion % 100);
}

bool validateHggcRuntimeAndDriver(Output *output, int *hggcRuntimeVersionOut, int *hggcDriverApiVersionOut, std::string &errorOut) {
    int runtimeVersion = 0;
    hggcError_t err = hggcRuntimeGetVersion(&runtimeVersion);
    if (err != hggcSuccess) {
        std::ostringstream msg;
        msg << "HGGC runtime version check failed: hggcRuntimeGetVersion returned " << hggcGetErrorString(err)
            << " (" << static_cast<int>(err) << ")";
        errorOut = msg.str();
        return false;
    }

    if (runtimeVersion < HGGCRT_VERSION) {
        std::ostringstream msg;
        msg << "Warning: HGGC runtime library version " << formatHggcVersionCode(runtimeVersion)
            << " is older than the toolkit sailbandwidth was built against ("
            << formatHggcVersionCode(HGGCRT_VERSION) << "). "
            << "Results may be unexpected if required runtime symbols are absent.";
        output->recordWarning(msg.str());
    }

    int driverVersion = 0;
    err = hggcDriverGetVersion(&driverVersion);
    if (err != hggcSuccess) {
        std::ostringstream msg;
        msg << "HGGC driver version check failed: hggcDriverGetVersion returned " << hggcGetErrorString(err)
            << " (" << static_cast<int>(err) << ")";
        errorOut = msg.str();
        return false;
    }

    if (driverVersion < HGGCRT_VERSION) {
        std::ostringstream msg;
        msg << "Warning: driver reports maximum supported HGGC version "
            << formatHggcVersionCode(driverVersion)
            << ", which is below the toolkit sailbandwidth was built against ("
            << formatHggcVersionCode(HGGCRT_VERSION) << "). "
            << "Results may be unexpected if required driver APIs are unavailable.";
        output->recordWarning(msg.str());
    }

    if (hggcRuntimeVersionOut) {
        *hggcRuntimeVersionOut = runtimeVersion;
    }
    if (hggcDriverApiVersionOut) {
        *hggcDriverApiVersionOut = driverVersion;
    }
    return true;
}
