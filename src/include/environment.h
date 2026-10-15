/*
 * SPDX-FileCopyrightText: Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#ifndef ENVIRONMENT_H_
#define ENVIRONMENT_H_

#include <memory>
#include <string>

#ifdef MULTINODE
#include <mpi.h>
#endif

class Environment {
 public:
    virtual ~Environment() = default;
    virtual void initialize(int argc, char** argv) = 0;
    virtual void finalize() = 0;
    virtual int getRank() const = 0;
    virtual int getSize() const = 0;
    virtual int getLocalRank() const = 0;
    virtual std::string getHostname() const = 0;

    static std::unique_ptr<Environment> create(int argc, char** argv);
};

class SingleNodeEnv : public Environment {
 public:
    void initialize(int argc, char** argv) override {}
    void finalize() override {}
    int getRank() const override { return 0; }
    int getSize() const override { return 1; }
    int getLocalRank() const override { return 0; }
    std::string getHostname() const override;
};

#ifdef MULTINODE
class MultiNodeEnv : public Environment {
 private:
    int rank = 0;
    int size = 1;
    int localRank = 0;
    std::string hostname;

 public:
    void initialize(int argc, char** argv) override;
    void finalize() override;
    int getRank() const override { return rank; }
    int getSize() const override { return size; }
    int getLocalRank() const override { return localRank; }
    std::string getHostname() const override { return hostname; }
};
#endif

extern std::unique_ptr<Environment> env;

#endif  // ENVIRONMENT_H_
