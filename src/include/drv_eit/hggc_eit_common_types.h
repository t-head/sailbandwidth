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

#ifndef __HGGC_EIT_COMMON_TYPES__
#define __HGGC_EIT_COMMON_TYPES__

/// @brief EIT stream creation flags, super set of HGstream_flags.
enum HGeitStreamFlags {
  HG_EIT_STREAM_DEFAULT      = 0x0,        /**< Default stream flag */
  HG_EIT_STREAM_NON_BLOCKING = (1u <<  0), /**< Stream does not synchronize with stream 0 (the NULL stream) */
  HG_EIT_STREAM_COPY_ENGINE  = (1u << 16), /**< Stream prefers to performs memory processing on copy engine */
};

enum HGeitFuncAttribute {
  HG_EIT_FUNC_ATTRIBUTE_KERNEL_CONTROL   = 0,
  HG_EIT_FUNC_ATTRIBUTE_DISPATCH_CU_MASK = 1,
  HG_EIT_FUNC_ATTRIBUTE_DISPATCH_STRTEGY = 200,
};

static_assert(static_cast<uint32_t>(HG_STREAM_DEFAULT) == HG_EIT_STREAM_DEFAULT &&
              static_cast<uint32_t>(HG_STREAM_NON_BLOCKING) == HG_EIT_STREAM_NON_BLOCKING,
              "EIT stream flags should match the public stream flags!");

typedef struct HGversionInfo_st
{
    uint16_t major;
    uint16_t minor;
} HGversionInfo;

// driver: hggc; base: kmd internal version; platform: umd.
typedef struct HGdriverVersionInfo_st
{
    HGversionInfo driver;       /**< HGGC main driver library version */
    HGversionInfo platform;     /**< HGGC driver platform library version */
    HGversionInfo base;         /**< HGGC base driver ioctl version */
} HGdriverVersionInfo;

#endif // __HGGC_EIT_COMMON_TYPES__