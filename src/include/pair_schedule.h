/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved.
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

#ifndef PAIR_SCHEDULE_H_
#define PAIR_SCHEDULE_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using PairPhases = std::vector<std::vector<std::pair<int, int>>>;

PairPhases planPairPhases(const std::vector<std::pair<int, int>> &pairs, int rankCount, bool unorderedPairs,
                          std::size_t maxPairsPerPhase);
std::uint64_t pairPhasesDigest(const PairPhases &phases);

#endif  // PAIR_SCHEDULE_H_
