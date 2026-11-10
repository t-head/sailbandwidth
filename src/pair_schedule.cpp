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

#include "pair_schedule.h"

#include <algorithm>
#include <numeric>
#include <set>
#include <string>
#include <tuple>

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct RoundTable {
    int size = 0;
    std::vector<int> round;
    std::vector<char> leading;
};

RoundTable buildRoundTable(int size) {
    RoundTable table;
    table.size = size;
    table.round.assign(static_cast<std::size_t>(size) * size, -1);
    table.leading.assign(static_cast<std::size_t>(size) * size, 0);

    const int slots = size + (size % 2);
    std::vector<int> order(slots);
    std::iota(order.begin(), order.end(), 0);
    for (int round = 0; round + 1 < slots; round++) {
        for (int i = 0; i < slots / 2; i++) {
            const int first = order[i];
            const int second = order[slots - 1 - i];
            if (first < size && second < size) {
                table.round[static_cast<std::size_t>(first) * size + second] = round;
                table.round[static_cast<std::size_t>(second) * size + first] = round;
                table.leading[static_cast<std::size_t>(first) * size + second] = 1;
            }
        }
        std::rotate(order.begin() + 1, order.begin() + 2, order.end());
    }
    return table;
}

struct ScheduledPair {
    std::pair<int, int> pair;
    std::tuple<int, int, int, std::size_t> key;
};

}  // namespace

PairPhases planPairPhases(const std::vector<std::pair<int, int>> &pairs, int rankCount, bool unorderedPairs,
                          std::size_t maxPairsPerPhase) {
    const RoundTable rounds = buildRoundTable(rankCount);

    std::vector<ScheduledPair> scheduled;
    std::set<std::pair<int, int>> seen;
    for (std::size_t order = 0; order < pairs.size(); order++) {
        std::pair<int, int> pair = pairs[order];
        if (pair.first < 0 || pair.first >= rankCount || pair.second < 0 || pair.second >= rankCount ||
            pair.first == pair.second) {
            throw std::string("Invalid rank pair (" + std::to_string(pair.first) + ", " + std::to_string(pair.second) +
                              ") for the concurrent pair schedule");
        }
        if (unorderedPairs && pair.first > pair.second) {
            std::swap(pair.first, pair.second);
        }
        if (!seen.insert(pair).second) {
            continue;
        }

        const std::size_t cell = static_cast<std::size_t>(pair.first) * rankCount + pair.second;
        const int direction = (unorderedPairs || rounds.leading[cell]) ? 0 : 1;
        ScheduledPair entry;
        entry.pair = pair;
        entry.key = std::make_tuple(rounds.round[cell], direction, std::min(pair.first, pair.second), order);
        scheduled.push_back(entry);
    }

    std::sort(scheduled.begin(), scheduled.end(),
              [](const ScheduledPair &lhs, const ScheduledPair &rhs) { return lhs.key < rhs.key; });

    PairPhases phases;
    std::vector<std::vector<char>> busyRanks;
    for (const auto &entry : scheduled) {
        const int first = entry.pair.first;
        const int second = entry.pair.second;
        std::size_t phase = 0;
        for (; phase < phases.size(); phase++) {
            const bool full = maxPairsPerPhase != 0 && phases[phase].size() >= maxPairsPerPhase;
            if (!full && !busyRanks[phase][first] && !busyRanks[phase][second]) {
                break;
            }
        }
        if (phase == phases.size()) {
            phases.emplace_back();
            busyRanks.emplace_back(static_cast<std::size_t>(rankCount), 0);
        }
        phases[phase].push_back(entry.pair);
        busyRanks[phase][first] = 1;
        busyRanks[phase][second] = 1;
    }
    return phases;
}

std::uint64_t pairPhasesDigest(const PairPhases &phases) {
    std::uint64_t digest = kFnvOffsetBasis;
    auto mix = [&digest](std::uint64_t value) {
        for (int byte = 0; byte < 8; byte++) {
            digest ^= (value >> (byte * 8)) & 0xffULL;
            digest *= kFnvPrime;
        }
    };
    mix(phases.size());
    for (const auto &phase : phases) {
        mix(phase.size());
        for (const auto &pair : phase) {
            mix(static_cast<std::uint32_t>(pair.first));
            mix(static_cast<std::uint32_t>(pair.second));
        }
    }
    return digest;
}
