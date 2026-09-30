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

#ifndef __HGGC_UUID_H__
#define __HGGC_UUID_H__

#include "hggc.h"

#define HG_UUID_CHAR(x) (char)((unsigned char)((x) & 0xff))

#define HG_DEFINE_UUID(var, timeLow, timeMid, timeHigh, varSeq, node) \
  static const HGuuid var =                                           \
  {                                                                   \
    {                                                                 \
      HG_UUID_CHAR((timeLow)  >> 24), HG_UUID_CHAR((timeLow)  >> 16), \
      HG_UUID_CHAR((timeLow)  >> 8 ), HG_UUID_CHAR((timeLow)  >> 0 ), \
      HG_UUID_CHAR((timeMid)  >> 8 ), HG_UUID_CHAR((timeMid)  >> 0 ), \
      HG_UUID_CHAR((timeHigh) >> 8 ), HG_UUID_CHAR((timeHigh) >> 0 ), \
      HG_UUID_CHAR((varSeq)   >> 8 ), HG_UUID_CHAR((varSeq)   >> 0 ), \
      HG_UUID_CHAR((node)     >> 40), HG_UUID_CHAR((node)     >> 32), \
      HG_UUID_CHAR((node)     >> 24), HG_UUID_CHAR((node)     >> 16), \
      HG_UUID_CHAR((node)     >> 8 ), HG_UUID_CHAR((node)     >> 0 ), \
    }                                                                 \
  };

#endif // __HGGC_UUID_H__
