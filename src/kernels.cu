/*
 * SPDX-FileCopyrightText: Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "kernels.cuh"

union Pack128 {
  int4 i4;
  uint4 u4;
  ulong2 l2;
};

// local load cache policy
#define NUM_LD_CP 9
#define LD_VOLATILE 0
#define LD_CA 1 // kp0 cache in all level
#define LD_G 2  // kp1 (read only) read through L1, cache in L2 and LLC
#define LD_CG 3 // kp2 read through L1/L2, cache in LLC
#define LD_CV 4 // kp3 (don't cache), read through L1/L2/LLC bulk write policy
#define LD_CS 5
                /* kp4 (cache streaming), streaming cache replacement policy for current.     \
                   cache line last: kp5 last use cache replacement policy for current         \
                   cache line */
#define LD_CE 6
#define LD_BL 7 // kp6 (bypass local), bypass L1/L2
#define LD_BA 8 // kp7 (bypass all), bypass L1/L2/LLC bulk read policy

// local store cache policy
#define NUM_ST_CP 7
#define ST_VOLATILE 0
#define ST_WB 1 // kp0 write back, cache in L1
#define ST_CG 2 // kp2 write through L1/L2, cache in LLC
#define ST_WT 3 // kp3, write through L1/L2/LLC, write to memory
#define ST_CS 4
#define ST_BL 5 // kp6 (bypass local), bypass L1/L2
#define ST_BA 6 // kp7 (bypass all), bypass L1/L2/LLC bulk read policy

// remote load/store cache policy
#define NUM_RMT_CP 4
#define RMT_C 0        // LLC kp2
#define RMT_VOLATILE 1 // LLC kp2
#define RMT_NCP 2      // LLC kp3
#define RMT_BYPASS 3   // LLC kp7

// [ppu1.5] remote route mode
#define NUM_RTMD 3
#define RTMD_NORMAL 0
#define RTMD_PA 1
#define RTMD_AR 2

#define REPEAT_P2P_BULK(func, ld_cp, st_cp, rmt)                                                        \
    setCUMask(dev, reinterpret_cast<const void *>(func<ld_cp, st_cp, rmt>));                            \
    if (blocks == -1 || threads == -1) {                                                                \
        HGGC_ASSERT(hggcOccupancyMaxPotentialBlockSize(&blocks, &threads, func<ld_cp, st_cp, rmt>));    \
        blocks = (1024 / blocks) * blocks;                                                              \
        ADJUST_SM_COUNT_FOR_SINGLE_DIE(blocks);                                                         \
    }                                                                                                   \
    for (int r = 0; r < desc.loopCount; r++) {                                                          \
        func<ld_cp, st_cp, rmt><<<blocks, threads, 0, stream>>>(                                        \
            (Pack128 *)dst, (Pack128 *)src, desc.copySize / sizeof(Pack128));                           \
    }

#define SWITCH_P2P_BULK_RTM(FUNC, _LD_CP, _ST_CP, rmt)                          \
    if (sm >= 89) {                                                             \
        switch (rmt) {                                                          \
            case RTMD_NORMAL:                                                   \
                REPEAT_P2P_BULK(FUNC, _LD_CP, _ST_CP, RTMD_NORMAL) break;       \
            case RTMD_PA:                                                       \
                REPEAT_P2P_BULK(FUNC, _LD_CP, _ST_CP, RTMD_PA) break;           \
            case RTMD_AR:                                                       \
                REPEAT_P2P_BULK(FUNC, _LD_CP, _ST_CP, RTMD_AR) break;           \
            default:                                                            \
                printf("Error: unknown remote route mode %d\n", rmt);           \
        }                                                                       \
    } else {                                                                    \
        if (rmt == -1) {                                                        \
            REPEAT_P2P_BULK(FUNC, _LD_CP, _ST_CP, -1) break;                    \
        } else {                                                                \
            REPEAT_P2P_BULK(FUNC, _LD_CP, _ST_CP, 0) break;                     \
        }                                                                       \
    }

#define SWITCH_P2P_BULK_LOAD_RTM(FUNC, ld_cp, _ST_CP, rmt)                      \
    switch (ld_cp) {                                                            \
        case RMT_C:                                                             \
            SWITCH_P2P_BULK_RTM(FUNC, RMT_C, _ST_CP, rmt) break;                \
        case RMT_VOLATILE:                                                      \
            SWITCH_P2P_BULK_RTM(FUNC, RMT_VOLATILE, _ST_CP, rmt) break;         \
        case RMT_NCP:                                                           \
            SWITCH_P2P_BULK_RTM(FUNC, RMT_NCP, _ST_CP, rmt) break;              \
        case RMT_BYPASS:                                                        \
            SWITCH_P2P_BULK_RTM(FUNC, RMT_BYPASS, _ST_CP, rmt) break;           \
        default:                                                                \
            printf("Error: unknown load cache policy %d\n", ld_cp);             \
    }                                                                           \

#define SWITCH_P2P_BULK_LOAD_KERN1(FUNC, ld_cp, _ST_CP, rmt)                    \
    if (rmt < 0) {                                                              \
        switch (ld_cp) {                                                        \
            case LD_VOLATILE:                                                   \
                REPEAT_P2P_BULK(FUNC, LD_VOLATILE, _ST_CP, -1) break;           \
            case LD_CA:                                                         \
                REPEAT_P2P_BULK(FUNC, LD_CA, _ST_CP, -1) break;                 \
            case LD_G:                                                          \
                REPEAT_P2P_BULK(FUNC, LD_G, _ST_CP, -1) break;                  \
            case LD_CG:                                                         \
                REPEAT_P2P_BULK(FUNC, LD_CG, _ST_CP, -1) break;                 \
            case LD_CV:                                                         \
                REPEAT_P2P_BULK(FUNC, LD_CV, _ST_CP, -1) break;                 \
            case LD_CS:                                                         \
                REPEAT_P2P_BULK(FUNC, LD_CS, _ST_CP, -1) break;                 \
            case LD_CE:                                                         \
                REPEAT_P2P_BULK(FUNC, LD_CE, _ST_CP, -1) break;                 \
            case LD_BL:                                                         \
                REPEAT_P2P_BULK(FUNC, LD_BL, _ST_CP, -1) break;                 \
            case LD_BA:                                                         \
                REPEAT_P2P_BULK(FUNC, LD_BA, _ST_CP, -1) break;                 \
            default:                                                            \
                printf("Error: unknown load cache policy %d\n", ld_cp);         \
        }                                                                       \
    } else {                                                                    \
        SWITCH_P2P_BULK_LOAD_RTM(FUNC, ld_cp, _ST_CP, rmt)                      \
    }

#define SWITCH_P2P_BULK_LOAD_KERN(FUNC, ld_cp, st_cp, rmt)                      \
    switch (st_cp) {                                                            \
        case ST_VOLATILE:                                                       \
            SWITCH_P2P_BULK_LOAD_KERN1(FUNC, ld_cp, ST_VOLATILE, rmt) break;    \
        case ST_WB:                                                             \
            SWITCH_P2P_BULK_LOAD_KERN1(FUNC, ld_cp, ST_WB, rmt) break;          \
        case ST_CG:                                                             \
            SWITCH_P2P_BULK_LOAD_KERN1(FUNC, ld_cp, ST_CG, rmt) break;          \
        case ST_WT:                                                             \
            SWITCH_P2P_BULK_LOAD_KERN1(FUNC, ld_cp, ST_WT, rmt) break;          \
        case ST_CS:                                                             \
            SWITCH_P2P_BULK_LOAD_KERN1(FUNC, ld_cp, ST_CS, rmt) break;          \
        case ST_BL:                                                             \
            SWITCH_P2P_BULK_LOAD_KERN1(FUNC, ld_cp, ST_BL, rmt) break;          \
        case ST_BA:                                                             \
            SWITCH_P2P_BULK_LOAD_KERN1(FUNC, ld_cp, ST_BA, rmt) break;          \
        default:                                                                \
            printf("Error: unknown store cache policy %d\n", st_cp);            \
    }

#define SWITCH_P2P_BULK_STORE_RTM(FUNC, _LD_CP, st_cp, rmt)                     \
{                                                                               \
    switch (st_cp) {                                                            \
        case RMT_C:                                                             \
            SWITCH_P2P_BULK_RTM(FUNC, _LD_CP, RMT_C, rmt) break;                \
        case RMT_VOLATILE:                                                      \
            SWITCH_P2P_BULK_RTM(FUNC, _LD_CP, RMT_VOLATILE, rmt) break;         \
        case RMT_NCP:                                                           \
            SWITCH_P2P_BULK_RTM(FUNC, _LD_CP, RMT_NCP, rmt) break;              \
        case RMT_BYPASS:                                                        \
            SWITCH_P2P_BULK_RTM(FUNC, _LD_CP, RMT_BYPASS, rmt) break;           \
        default:                                                                \
            printf("Error: unknown store cache policy %d\n", st_cp);            \
    }                                                                           \
}

#define SWITCH_P2P_BULK_STORE_KERN1(FUNC, _LD_CP, st_cp, rmt)                   \
    if (rmt < 0) {                                                              \
        switch (st_cp) {                                                        \
            case ST_VOLATILE:                                                   \
                REPEAT_P2P_BULK(FUNC, _LD_CP, ST_VOLATILE, -1) break;           \
            case ST_WB:                                                         \
                REPEAT_P2P_BULK(FUNC, _LD_CP, ST_WB, -1) break;                 \
            case ST_CG:                                                         \
                REPEAT_P2P_BULK(FUNC, _LD_CP, ST_CG, -1) break;                 \
            case ST_WT:                                                         \
                REPEAT_P2P_BULK(FUNC, _LD_CP, ST_WT, -1) break;                 \
            case ST_CS:                                                         \
                REPEAT_P2P_BULK(FUNC, _LD_CP, ST_CS, -1) break;                 \
            case ST_BL:                                                         \
                REPEAT_P2P_BULK(FUNC, _LD_CP, ST_BL, -1) break;                 \
            case ST_BA:                                                         \
                REPEAT_P2P_BULK(FUNC, _LD_CP, ST_BA, -1) break;                 \
            default:                                                            \
                printf("Error: unknown store cache policy %d\n", st_cp);        \
        }                                                                       \
    } else {                                                                    \
        SWITCH_P2P_BULK_STORE_RTM(FUNC, _LD_CP, st_cp, rmt)                     \
    }

#define SWITCH_P2P_BULK_STORE_KERN(FUNC, ld_cp, st_cp, rmt)                     \
    switch (ld_cp) {                                                            \
        case LD_VOLATILE:                                                       \
            SWITCH_P2P_BULK_STORE_KERN1(FUNC, LD_VOLATILE, st_cp, rmt) break;   \
        case LD_CA:                                                             \
            SWITCH_P2P_BULK_STORE_KERN1(FUNC, LD_CA, st_cp, rmt) break;         \
        case LD_G:                                                              \
            SWITCH_P2P_BULK_STORE_KERN1(FUNC, LD_G, st_cp, rmt) break;          \
        case LD_CG:                                                             \
            SWITCH_P2P_BULK_STORE_KERN1(FUNC, LD_CG, st_cp, rmt) break;         \
        case LD_CV:                                                             \
            SWITCH_P2P_BULK_STORE_KERN1(FUNC, LD_CV, st_cp, rmt) break;         \
        case LD_CS:                                                             \
            SWITCH_P2P_BULK_STORE_KERN1(FUNC, LD_CS, st_cp, rmt) break;         \
        case LD_CE:                                                             \
            SWITCH_P2P_BULK_STORE_KERN1(FUNC, LD_CE, st_cp, rmt) break;         \
        case LD_BL:                                                             \
            SWITCH_P2P_BULK_STORE_KERN1(FUNC, LD_BL, st_cp, rmt) break;         \
        case LD_BA:                                                             \
            SWITCH_P2P_BULK_STORE_KERN1(FUNC, LD_BA, st_cp, rmt) break;         \
        default:                                                                \
            printf("Error: unknown load cache policy %d\n", ld_cp);             \
    }

template <int CACHE_POLICY>
inline __device__ void bulk_fetch128(Pack128 &v, const Pack128 *p) {
    if (CACHE_POLICY == LD_VOLATILE) {
        v.u4 = __ppu_global_load_bulk_volatile_b32x4((volatile const void *)(p));
    } else if (CACHE_POLICY == LD_CA) {
        v.u4 = __ppu_global_ldca_bulk_b32x4((const void *)(p));
    } else if (CACHE_POLICY == LD_G) {
        v.u4 = __ppu_global_ldg_bulk_b32x4((const void *)(p));
    } else if (CACHE_POLICY == LD_CG) {
        v.u4 = __ppu_global_ldcg_bulk_b32x4((const void *)(p));
    } else if (CACHE_POLICY == LD_CV) {
        v.u4 = __ppu_global_ldcv_bulk_b32x4((const void *)(p));
    } else if (CACHE_POLICY == LD_CS) {
        v.u4 = __ppu_global_ldcs_bulk_b32x4((const void *)(p));
    } else if (CACHE_POLICY == LD_CE) {
        v.u4 = __ppu_global_ldce_bulk_b32x4((const void *)(p));
    } else if (CACHE_POLICY == LD_BL) {
        v.u4 = __ppu_global_ldbl_bulk_b32x4((const void *)(p));
    } else if (CACHE_POLICY == LD_BA) {
        v.u4 = __ppu_global_ldba_bulk_b32x4((const void *)(p));
    }
}

template <int CACHE_POLICY, int RTMD>
inline __device__ void remote_fetch128(Pack128 &v, const Pack128 *p) {
#if __HGGC_ARCH__ >= 150
    if (CACHE_POLICY == RMT_C) {
        v.u4 = __ppu_remote_load_bulk_b32x4((const void *)(p), RTMD);
    } else if (CACHE_POLICY == RMT_VOLATILE) {
        v.u4 = __ppu_remote_load_bulk_volatile_b32x4((volatile const void *)(p), RTMD);
    } else if (CACHE_POLICY == RMT_NCP) {
        v.u4 = __ppu_remote_load_bulk_ncp_b32x4((const void *)(p), RTMD);
    } else if (CACHE_POLICY == RMT_BYPASS) {
        v.u4 = __ppu_remote_load_bulk_bypass_b32x4((const void *)(p), RTMD);
    }
#elif __HGGC_ARCH__ >= 100
    if (CACHE_POLICY == RMT_C) {
        v.u4 = __ppu_remote_load_bulk_b32x4((const void *)(p));
    } else if (CACHE_POLICY == RMT_VOLATILE) {
        v.u4 = __ppu_remote_load_bulk_volatile_b32x4((volatile const void *)(p));
    } else if (CACHE_POLICY == RMT_NCP) {
        v.u4 = __ppu_remote_load_bulk_ncp_b32x4((const void *)(p));
    } else if (CACHE_POLICY == RMT_BYPASS) {
        v.u4 = __ppu_remote_load_bulk_bypass_b32x4((const void *)(p));
    }
#endif
}

template <int CACHE_POLICY>
inline __device__ void bulk_store128(Pack128 *p, Pack128 &v) {
    if (CACHE_POLICY == ST_VOLATILE) {
        __ppu_global_store_bulk_volatile_b32x4((volatile void *)(p), v.u4);
    } else if (CACHE_POLICY == ST_WB) {
        __ppu_global_stwb_bulk_b32x4((void *)(p), v.u4);
    } else if (CACHE_POLICY == ST_CG) {
        __ppu_global_stcg_bulk_b32x4((void *)(p), v.u4);
    } else if (CACHE_POLICY == ST_WT) {
        __ppu_global_stwt_bulk_b32x4((void *)(p), v.u4);
    } else if (CACHE_POLICY == ST_CS) {
        __ppu_global_stcs_bulk_b32x4((void *)(p), v.u4);
    } else if (CACHE_POLICY == ST_BL) {
        __ppu_global_stbl_bulk_b32x4((void *)(p), v.u4);
    } else if (CACHE_POLICY == ST_BA) {
        __ppu_global_stba_bulk_b32x4((void *)(p), v.u4);
    }
}

template <int CACHE_POLICY, int RTMD>
inline __device__ void remote_store128(Pack128 *p, Pack128 &v) {
#if __HGGC_ARCH__ >= 150
    if (CACHE_POLICY == RMT_C) {
        __ppu_remote_store_bulk_b32x4((void *)(p), v.u4, RTMD);
    } else if (CACHE_POLICY == RMT_VOLATILE) {
        __ppu_remote_store_bulk_volatile_b32x4((volatile void *)(p), v.u4, RTMD);
    } else if (CACHE_POLICY == RMT_NCP) {
        __ppu_remote_store_bulk_ncp_b32x4((void *)(p), v.u4, RTMD);
    } else if (CACHE_POLICY == RMT_BYPASS) {
        __ppu_remote_store_bulk_bypass_b32x4((void *)(p), v.u4, RTMD);
    }
#elif __HGGC_ARCH__ >= 100
    if (CACHE_POLICY == RMT_C) {
        __ppu_remote_store_bulk_b32x4((void *)(p), v.u4);
    } else if (CACHE_POLICY == RMT_VOLATILE) {
        __ppu_remote_store_bulk_volatile_b32x4((volatile void *)(p), v.u4);
    } else if (CACHE_POLICY == RMT_NCP) {
        __ppu_remote_store_bulk_ncp_b32x4((void *)(p), v.u4);
    } else if (CACHE_POLICY == RMT_BYPASS) {
        __ppu_remote_store_bulk_bypass_b32x4((void *)(p), v.u4);
    }
#endif
}

#define BULK_V4_DATA_SIZE 512
#define BULK_V4_ELEM_LEN (BULK_V4_DATA_SIZE / 16)

template <int LD_CP, int ST_CP, int RTMD>
__global__ void copyp2p_load_bulk_v4(Pack128 *__restrict__ dest,
                                     Pack128 *__restrict__ src,
                                     int num_elems) {
    int warp = threadIdx.x / WARP_SIZE;
    int nwarps_per_block = blockDim.x / WARP_SIZE;
    int bid = blockIdx.x;
    int nblocks = gridDim.x;
    int loop_size = BULK_V4_ELEM_LEN * nwarps_per_block * nblocks;
    Pack128 val;
    int offset = (bid * nwarps_per_block + warp) * BULK_V4_ELEM_LEN;
    while (offset < num_elems) {
        if (RTMD < 0) {
            bulk_fetch128<LD_CP>(val, src + offset);
        } else {
            remote_fetch128<LD_CP, RTMD>(val, src + offset);
        }
        bulk_store128<ST_CP>(dest + offset, val);
        offset += loop_size;
    }
}

template <int LD_CP, int ST_CP, int RTMD>
__global__ void copyp2p_store_bulk_v4(Pack128 *__restrict__ dest,
                                      Pack128 *__restrict__ src,
                                      int num_elems) {
    int warp = threadIdx.x / WARP_SIZE;
    int nwarps_per_block = blockDim.x / WARP_SIZE;
    int bid = blockIdx.x;
    int nblocks = gridDim.x;
    int loop_size = BULK_V4_ELEM_LEN * nwarps_per_block * nblocks;
    Pack128 val;
    int offset = (bid * nwarps_per_block + warp) * BULK_V4_ELEM_LEN;
    while (offset < num_elems) {
        bulk_fetch128<LD_CP>(val, src + offset);
        if (RTMD < 0) {
            bulk_store128<ST_CP>(dest + offset, val);
        } else {
            remote_store128<ST_CP, RTMD>(dest + offset, val);
        }
        offset += loop_size;
    }
}

__global__ void simpleCopyKernel(unsigned long long loopCount, uint4 *dst, uint4 *src) {
    for (unsigned int i = 0; i < loopCount; i++) {
        const int idx = blockIdx.x * blockDim.x + threadIdx.x;
        size_t offset = idx * sizeof(uint4);
        uint4* dst_uint4 = reinterpret_cast<uint4*>((char*)dst + offset);
        uint4* src_uint4 = reinterpret_cast<uint4*>((char*)src + offset);
        __stcg(dst_uint4, __ldcg(src_uint4));
    }
}

__global__ void stridingMemcpyKernel(unsigned int totalThreadCount, unsigned long long loopCount, uint4* dst, uint4* src, size_t chunkSizeInElement) {
    unsigned long long from = blockDim.x * blockIdx.x + threadIdx.x;
    unsigned long long bigChunkSizeInElement = chunkSizeInElement / 12;
    dst += from;
    src += from;
    uint4* dstBigEnd = dst + (bigChunkSizeInElement * 12) * totalThreadCount;
    uint4* dstEnd = dst + chunkSizeInElement * totalThreadCount;

    for (unsigned int i = 0; i < loopCount; i++) {
        uint4* cdst = dst;
        uint4* csrc = src;

        while (cdst < dstBigEnd) {
            uint4 pipe_0 = *csrc; csrc += totalThreadCount;
            uint4 pipe_1 = *csrc; csrc += totalThreadCount;
            uint4 pipe_2 = *csrc; csrc += totalThreadCount;
            uint4 pipe_3 = *csrc; csrc += totalThreadCount;
            uint4 pipe_4 = *csrc; csrc += totalThreadCount;
            uint4 pipe_5 = *csrc; csrc += totalThreadCount;
            uint4 pipe_6 = *csrc; csrc += totalThreadCount;
            uint4 pipe_7 = *csrc; csrc += totalThreadCount;
            uint4 pipe_8 = *csrc; csrc += totalThreadCount;
            uint4 pipe_9 = *csrc; csrc += totalThreadCount;
            uint4 pipe_10 = *csrc; csrc += totalThreadCount;
            uint4 pipe_11 = *csrc; csrc += totalThreadCount;

            *cdst = pipe_0; cdst += totalThreadCount;
            *cdst = pipe_1; cdst += totalThreadCount;
            *cdst = pipe_2; cdst += totalThreadCount;
            *cdst = pipe_3; cdst += totalThreadCount;
            *cdst = pipe_4; cdst += totalThreadCount;
            *cdst = pipe_5; cdst += totalThreadCount;
            *cdst = pipe_6; cdst += totalThreadCount;
            *cdst = pipe_7; cdst += totalThreadCount;
            *cdst = pipe_8; cdst += totalThreadCount;
            *cdst = pipe_9; cdst += totalThreadCount;
            *cdst = pipe_10; cdst += totalThreadCount;
            *cdst = pipe_11; cdst += totalThreadCount;
        }

        while (cdst < dstEnd) {
            *cdst = *csrc; cdst += totalThreadCount; csrc += totalThreadCount;
        }
    }
}

// This kernel performs a split warp copy, alternating copy directions across warps.
__global__ void splitWarpCopyKernel(unsigned long long loopCount, uint4 *dst, uint4 *src) {
    for (unsigned int i = 0; i < loopCount; i++) {
        unsigned int idx = blockIdx.x * blockDim.x + threadIdx.x;
        unsigned int globalWarpId = idx / warpSize;
        unsigned int warpLaneId = idx % warpSize;
        uint4* dst_uint4;
        uint4* src_uint4;

        // alternate copy directions across warps
        if (globalWarpId & 0x1) {
            // odd warp
            dst_uint4 = dst + (globalWarpId * warpSize + warpLaneId);
            src_uint4 = src + (globalWarpId * warpSize + warpLaneId);
        } else {
            // even warp
            dst_uint4 = src + (globalWarpId * warpSize + warpLaneId);
            src_uint4 = dst + (globalWarpId * warpSize + warpLaneId);
        }

        __stcg(dst_uint4, __ldcg(src_uint4));
    }
}

__global__ void ptrChasingKernel(struct LatencyNode *data, size_t size, unsigned int accesses, unsigned int targetBlock) {
    struct LatencyNode *p = data;
    if (blockIdx.x != targetBlock) return;
    for (auto i = 0; i < accesses; ++i) {
        p = p->next;
    }

    // avoid compiler optimization
    if (p == nullptr) {
        __trap();
    }
}

static __device__ __noinline__
void mc_st_u32(unsigned int *dst, unsigned int v) {
#if __HGGC_ARCH__ >= 900
    asm volatile ("multimem.st.u32 [%0], %1;" :: "l"(dst), "r" (v));
#endif
}

static __device__ __noinline__
void mc_ld_u32(unsigned int *dst, const unsigned int *src) {
#if __HGGC_ARCH__ >= 900
     asm volatile ("multimem.ld_reduce.and.b32 %0, [%1];" : "=r"((*dst)) : "l" (src));
#endif
}

// Writes from regular memory to multicast memory
__global__ void multicastCopyKernel(unsigned long long loopCount, unsigned int* __restrict__ dst, unsigned int* __restrict__ src, size_t nElems) {
    const size_t totalThreadCount = blockDim.x * gridDim.x;
    const size_t offset = blockDim.x * blockIdx.x + threadIdx.x;
    unsigned int* const enddst = dst + nElems;
    dst += offset;
    src += offset;

    for (unsigned int i = 0; i < loopCount; i++) {
        // Reset pointers to src and dst chunks.
        unsigned int* cur_src_ptr = src;
        unsigned int* cur_dst_ptr = dst;
        #pragma unroll 12
        while (cur_dst_ptr < enddst) {
            mc_st_u32(cur_dst_ptr, *cur_src_ptr);
            cur_dst_ptr += totalThreadCount;
            cur_src_ptr += totalThreadCount;
        }
    }
}

double latencyPtrChaseKernel(const int srcId, void* data, size_t size, unsigned long long latencyMemAccessCnt, unsigned smCount) {
    HGstream stream;
    int device, clock_rate_khz;
    double latencySum = 0.0f, finalLatencyPerAccessNs = 0.0;
    HGcontext srcCtx;
    hggcEvent_t start, end;
    float latencyMs = 0;

    HGGC_ASSERT(hggcEventCreate(&start));
    HGGC_ASSERT(hggcEventCreate(&end));

    HG_ASSERT(hgDevicePrimaryCtxRetain(&srcCtx, srcId));
    HG_ASSERT(hgCtxSetCurrent(srcCtx));

    HG_ASSERT(hgStreamCreate(&stream, HG_STREAM_DEFAULT));
    HG_ASSERT(hgCtxGetDevice(&device));
    HG_ASSERT(hgDeviceGetAttribute(&clock_rate_khz, HG_DEVICE_ATTRIBUTE_CLOCK_RATE, device));

    setCUMask(device, reinterpret_cast<const void *>(ptrChasingKernel));
    int blocks = nBlocks;
    if (blocks == -1) {
        blocks = smCount;
        ADJUST_SM_COUNT_FOR_SINGLE_DIE(blocks);
    }
    for (int targetBlock = 0; targetBlock < blocks; ++targetBlock) {
        HGGC_ASSERT(hggcEventRecord(start, stream));
        ptrChasingKernel<<<blocks, 1, 0, stream>>> ((struct LatencyNode*) data, size, latencyMemAccessCnt / blocks, targetBlock);
        HGGC_ASSERT(hggcEventRecord(end, stream));
        HGGC_ASSERT(hggcGetLastError());
        HG_ASSERT(hgStreamSynchronize(stream));
        hggcEventElapsedTime(&latencyMs, start, end);
        latencySum += (latencyMs / 1000);
    }
    finalLatencyPerAccessNs = (latencySum * 1.0E9) / (latencyMemAccessCnt);

    HGGC_ASSERT(hggcEventDestroy(start));
    HGGC_ASSERT(hggcEventDestroy(end));

    return finalLatencyPerAccessNs;
}

size_t copyKernel(MemcpyDescriptor &desc) {
    HGdevice dev;
    HGcontext ctx;
    HGstream stream = desc.mainstream;
    // Keep auto-derived launch geometry local so it cannot leak into later testcases.
    int blocks = nBlocks, threads = nThreads;

    HG_ASSERT(hgStreamGetCtx(stream, &ctx));
    HG_ASSERT(hgCtxGetDevice(&dev));

    HGdeviceptr dst = desc.dstBuffer->getBuffer();
    HGdeviceptr src = desc.srcBuffer->getBuffer();
    bool isD2D = desc.dstBuffer->isDeviceBuffer() && desc.srcBuffer->isDeviceBuffer();
    bool isWrite = false;
    bool isLocalCopy = false;
    if (desc.dstBuffer->getMPIRank() == -1 && desc.srcBuffer->getMPIRank() == -1) {
        isWrite = desc.srcBuffer->getPrimaryCtx() == ctx;
        isLocalCopy = desc.dstBuffer->getBufferIdx() == desc.srcBuffer->getBufferIdx();
    }
#ifdef MULTINODE
    else {
        isWrite = (worldRank == desc.srcBuffer->getMPIRank());
        isLocalCopy = desc.dstBuffer->getMPIRank() == desc.srcBuffer->getMPIRank();
    }
#endif

    if (useNormalCopy || !isD2D) {
        // Buffers smaller than the 64 MiB small-buffer threshold use the simple
        // copy kernel so the buffer size is not truncated; buffers at or above
        // the threshold use the optimized kernel.
        if (desc.copySize < (smallBufferThreshold * _MiB)) {
            // copy size is rounded down to 16 bytes
            unsigned int numUint4 = desc.copySize / sizeof(uint4);
            // we allow max 1024 threads per block, and then scale out the copy across multiple blocks
            dim3 block(std::min(numUint4, static_cast<unsigned int>(1024)));
            dim3 grid(numUint4/block.x);
            setCUMask(dev, reinterpret_cast<const void *>(simpleCopyKernel));
            simpleCopyKernel<<<grid, block, 0 , stream>>> (desc.loopCount, (uint4 *)dst, (uint4 *)src);
            return numUint4 * sizeof(uint4);
        }

        if (blocks == -1 || threads == -1) {
            HG_ASSERT(hgDeviceGetAttribute(&blocks, HG_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev));
            ADJUST_SM_COUNT_FOR_SINGLE_DIE(blocks);
            threads = numThreadPerBlock;
        }

        unsigned int totalThreadCount = blocks * threads;
        // adjust size to elements (size is multiple of MB, so no truncation here)
        size_t sizeInElement = desc.copySize / sizeof(uint4);
        // this truncates the copy
        sizeInElement = totalThreadCount * (sizeInElement / totalThreadCount);
        size_t chunkSizeInElement = sizeInElement / totalThreadCount;

        setCUMask(dev, reinterpret_cast<const void *>(stridingMemcpyKernel));
        stridingMemcpyKernel<<<blocks, threads, 0, stream>>> (totalThreadCount, desc.loopCount, (uint4 *)dst, (uint4 *)src, chunkSizeInElement);

        return sizeInElement * sizeof(uint4);
    } else {
        int ldCp = LD_VOLATILE, stCp = ST_VOLATILE, rmtCp = RMT_VOLATILE, rtmd = RTMD_PA;

        if (isWrite) {
            int realLdCp = ldCp;
            int realStCp = isLocalCopy ? stCp : rmtCp;
            int rmt = (isLocalCopy) ? -1 : ((sm < 89) ? 0 : rtmd);

            SWITCH_P2P_BULK_STORE_KERN(copyp2p_store_bulk_v4, realLdCp, realStCp, rmt)
        } else {
            int realLdCp = isLocalCopy ? ldCp : rmtCp;;
            int realStCp = stCp;
            int rmt = (isLocalCopy) ? -1 : ((sm < 89) ? 0 : rtmd);

            SWITCH_P2P_BULK_LOAD_KERN(copyp2p_load_bulk_v4, realLdCp, realStCp, rmt)
        }

        return desc.copySize;
    }
}

size_t copyKernelSplitWarp(MemcpyDescriptor &desc) {
    HGdevice dev;
    HGcontext ctx;

    HG_ASSERT(hgStreamGetCtx(desc.mainstream, &ctx));
    HG_ASSERT(hgCtxGetDevice(&dev));

    // copy size is rounded down to 16 bytes
    unsigned int numUint4 = desc.copySize / sizeof(uint4);

    // we allow max 1024 threads per block, and then scale out the copy across multiple blocks
    dim3 block(std::min(numUint4, static_cast<unsigned int>(1024)));
    dim3 grid(numUint4/block.x);
    setCUMask(dev, reinterpret_cast<const void *>(splitWarpCopyKernel));
    splitWarpCopyKernel<<<grid, block, 0 , desc.mainstream>>> (desc.loopCount, (uint4 *)desc.dstBuffer->getBuffer(), (uint4 *)desc.srcBuffer->getBuffer());
    return numUint4 * sizeof(uint4);
}

size_t multicastCopy(HGdeviceptr dstBuffer, HGdeviceptr srcBuffer, size_t size, HGstream stream, unsigned long long loopCount) {
    HGdevice dev;
    HGcontext ctx;

    HG_ASSERT(hgStreamGetCtx(stream, &ctx));
    HG_ASSERT(hgCtxGetDevice(&dev));

    int blocks = nBlocks, threads = nThreads;
    if (blocks == -1 || threads == -1) {
        HG_ASSERT(hgDeviceGetAttribute(&blocks, HG_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev));
        ADJUST_SM_COUNT_FOR_SINGLE_DIE(blocks);
        threads = numThreadPerBlock;
    }

    // adjust size to elements (size is multiple of MB, so no truncation here)
    size_t sizeInElement = size / sizeof(unsigned);
    setCUMask(dev, reinterpret_cast<const void *>(multicastCopyKernel));
    multicastCopyKernel<<<blocks, threads, 0, stream>>> (loopCount, (unsigned *)dstBuffer, (unsigned *)srcBuffer, sizeInElement);
    return sizeInElement * sizeof(unsigned);
}

__global__ void spinKernelDevice(volatile int *latch, const unsigned long long timeoutClocks) {
    unsigned long long endTime = clock64() + timeoutClocks;
    while (!*latch) {
        if (timeoutClocks != ~0ULL && clock64() > endTime) {
            break;
        }
    }
}

HGresult spinKernel(volatile int *latch, HGstream mainstream, HGstream die0stream, HGstream die1stream, unsigned long long timeoutMs) {
    int clocksPerMs = 0;
    HGcontext ctx;
    HGdevice dev;
    if (mainstream == nullptr) {
        HG_ASSERT(hgStreamGetCtx(die0stream, &ctx));
        HG_ASSERT(hgCtxGetDevice(&dev));
        HG_ASSERT(hgDeviceGetAttribute(&clocksPerMs, HG_DEVICE_ATTRIBUTE_CLOCK_RATE, dev));

        unsigned long long timeoutClocks = clocksPerMs * timeoutMs;
        spinKernelDevice<<<1, 1, 0, die0stream>>>(latch, timeoutClocks);
        spinKernelDevice<<<1, 1, 0, die1stream>>>(latch, timeoutClocks);
    } else {
        HG_ASSERT(hgStreamGetCtx(mainstream, &ctx));
        HG_ASSERT(hgCtxGetDevice(&dev));
        HG_ASSERT(hgDeviceGetAttribute(&clocksPerMs, HG_DEVICE_ATTRIBUTE_CLOCK_RATE, dev));

        unsigned long long timeoutClocks = clocksPerMs * timeoutMs;
        spinKernelDevice<<<1, 1, 0, mainstream>>>(latch, timeoutClocks);
    }


    return HGGC_SUCCESS;
}

__global__ void spinKernelDeviceMultistage(volatile int *latch1, volatile int *latch2, const unsigned long long timeoutClocks) {
    if (latch1) {
        unsigned long long endTime = clock64() + timeoutClocks;
        while (!*latch1) {
            if (timeoutClocks != ~0ULL && clock64() > endTime) {
                return;
            }
        }

        *latch2 = 1;
    }

    unsigned long long endTime = clock64() + timeoutClocks;
    while (!*latch2) {
        if (timeoutClocks != ~0ULL && clock64() > endTime) {
            break;
        }
    }
}

// Implement a 2-stage spin kernel for multi-node synchronization.
// One of the host nodes releases the first latch. Subsequently,
// the second latch is released, that is polled by all other devices
// latch1 argument is optional. If defined, kernel will spin on it until released, and then will release latch2.
// latch2 argument is mandatory. Kernel will spin on it until released.
// timeoutMs argument applies to each stage separately.
// However, since each kernel will spin on only one stage, total runtime is still limited by timeoutMs
HGresult spinKernelMultistage(volatile int *latch1, volatile int *latch2, HGstream mainstream, HGstream die0stream, HGstream die1stream, unsigned long long timeoutMs) {
    int clocksPerMs = 0;
    HGcontext ctx;
    HGdevice dev;

    ASSERT(latch2 != nullptr);

    if (mainstream == nullptr) {
        HG_ASSERT(hgStreamGetCtx(die0stream, &ctx));
        HG_ASSERT(hgCtxGetDevice(&dev));
        HG_ASSERT(hgDeviceGetAttribute(&clocksPerMs, HG_DEVICE_ATTRIBUTE_CLOCK_RATE, dev));

        unsigned long long timeoutClocks = clocksPerMs * timeoutMs;
        spinKernelDeviceMultistage<<<1, 1, 0, die0stream>>>(latch1, latch2, timeoutClocks);
        spinKernelDeviceMultistage<<<1, 1, 0, die1stream>>>(latch1, latch2, timeoutClocks);
    } else {
        HG_ASSERT(hgStreamGetCtx(mainstream, &ctx));
        HG_ASSERT(hgCtxGetDevice(&dev));
        HG_ASSERT(hgDeviceGetAttribute(&clocksPerMs, HG_DEVICE_ATTRIBUTE_CLOCK_RATE, dev));

        unsigned long long timeoutClocks = clocksPerMs * timeoutMs;
        spinKernelDeviceMultistage<<<1, 1, 0, mainstream>>>(latch1, latch2, timeoutClocks);
    }

    return HGGC_SUCCESS;
}

__global__ void memsetKernelDevice(HGdeviceptr buffer, HGdeviceptr pattern, unsigned long long num_elements, unsigned int num_pattern_elements) {
    unsigned long long idx = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned int* buf = reinterpret_cast<unsigned int*>(buffer);
    unsigned int* pat = reinterpret_cast<unsigned int*>(pattern);

    if (idx < num_elements) {
        buf[idx] = pat[idx % num_pattern_elements];
    }
}

// This kernel clears memory locations in the buffer based on warp parity.
// If clearOddWarpIndexed is true, it clears buffer locations indexed by odd warps.
// Otherwise, it clears buffer locations indexed by even warps.
__global__ void memclearKernelByWarpParityDevice(HGdeviceptr buffer, bool clearOddWarpIndexed) {
    unsigned int idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint4* buf = reinterpret_cast<uint4*>(buffer);
    unsigned int globalWarpId = idx / warpSize;
    unsigned int thread_idx_in_warp = idx % warpSize;

    if (clearOddWarpIndexed) {
        // clear memory locations in buffer indexed by odd warps
        if (globalWarpId & 0x1) {
            buf[globalWarpId * warpSize + thread_idx_in_warp] = make_uint4(0x0, 0x0, 0x0, 0x0);
        }
    } else {
        // clear memory locations in buffer indexed by even warps
        if (!(globalWarpId & 0x1)) {
            buf[globalWarpId * warpSize + thread_idx_in_warp] = make_uint4(0x0, 0x0, 0x0, 0x0);
        }
    }
}

__global__ void memcmpKernelDevice(HGdeviceptr buffer, HGdeviceptr pattern, unsigned long long num_elements, unsigned int num_pattern_elements, HGdeviceptr errorFlag) {
    unsigned long long idx = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned int* buf = reinterpret_cast<unsigned int*>(buffer);
    unsigned int* pat = reinterpret_cast<unsigned int*>(pattern);

    if (idx < num_elements) {
        if (buf[idx] != pat[idx % num_pattern_elements]) {
            atomicExch((int*)errorFlag, 1);
        }
    }
}

__global__ void multicastMemcmpKernelDevice(HGdeviceptr buffer, HGdeviceptr pattern, unsigned long long num_elements, unsigned int num_pattern_elements, HGdeviceptr errorFlag) {
    unsigned long long idx = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned int* buf = reinterpret_cast<unsigned int*>(buffer);
    unsigned int* pat = reinterpret_cast<unsigned int*>(pattern);

    if (idx < num_elements) {
        unsigned buf_val;
        mc_ld_u32(&buf_val, &buf[idx]);
        if (buf_val != pat[idx % num_pattern_elements]) {
            atomicExch((int*)errorFlag, 1);
        }
    }
}

HGresult memsetKernel(HGstream stream, HGdeviceptr buffer, HGdeviceptr pattern, unsigned long long num_elements, unsigned int num_pattern_elements) {
    unsigned threadsPerBlock = 1024;
    unsigned long long blocks = (num_elements + threadsPerBlock - 1) / threadsPerBlock;

    memsetKernelDevice<<<blocks, threadsPerBlock, 0, stream>>>(buffer, pattern, num_elements, num_pattern_elements);
    HGGC_ASSERT(hggcGetLastError());
    return HGGC_SUCCESS;
}

HGresult memclearKernelByWarpParity(HGstream stream, HGdeviceptr buffer, size_t size, bool clearOddWarpIndexed) {
    HGdevice dev;
    HGcontext ctx;

    HG_ASSERT(hgStreamGetCtx(stream, &ctx));
    HG_ASSERT(hgCtxGetDevice(&dev));

    int numSm;
    HG_ASSERT(hgDeviceGetAttribute(&numSm, HG_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev));
    // copy size is rounded down to 16 bytes
    unsigned int numUint4 = size / sizeof(uint4);

    // we allow max 1024 threads per block, and then scale out the copy across multiple blocks
    dim3 block(std::min(numUint4, static_cast<unsigned int>(1024)));

    dim3 grid(numUint4/block.x);
    memclearKernelByWarpParityDevice<<<grid, block, 0 , stream>>> (buffer, clearOddWarpIndexed);
    HGGC_ASSERT(hggcGetLastError());
    return HGGC_SUCCESS;
}

HGresult memcmpKernel(HGstream stream, HGdeviceptr buffer, HGdeviceptr pattern, unsigned long long num_elements, unsigned int num_pattern_elements, HGdeviceptr errorFlag) {
    unsigned threadsPerBlock = 1024;
    unsigned long long blocks = (num_elements + threadsPerBlock - 1) / threadsPerBlock;

    memcmpKernelDevice<<<blocks, threadsPerBlock, 0, stream>>>(buffer, pattern, num_elements, num_pattern_elements, errorFlag);
    HGGC_ASSERT(hggcGetLastError());
    return HGGC_SUCCESS;
}

HGresult multicastMemcmpKernel(HGstream stream, HGdeviceptr buffer, HGdeviceptr pattern, unsigned long long num_elements, unsigned int num_pattern_elements, HGdeviceptr errorFlag) {
    unsigned threadsPerBlock = 1024;
    unsigned long long blocks = (num_elements + threadsPerBlock - 1) / threadsPerBlock;

    multicastMemcmpKernelDevice<<<blocks, threadsPerBlock, 0, stream>>>(buffer, pattern, num_elements, num_pattern_elements, errorFlag);
    HGGC_ASSERT(hggcGetLastError());
    return HGGC_SUCCESS;
}

void preloadKernels(int deviceCount) {
    hggcFuncAttributes unused;
    for (int iDev = 0; iDev < deviceCount; iDev++) {
        hggcSetDevice(iDev);
        hggcFuncGetAttributes(&unused, &stridingMemcpyKernel);
        hggcFuncGetAttributes(&unused, &spinKernelDevice);
        hggcFuncGetAttributes(&unused, &spinKernelDeviceMultistage);
        hggcFuncGetAttributes(&unused, &simpleCopyKernel);
        hggcFuncGetAttributes(&unused, &splitWarpCopyKernel);
        hggcFuncGetAttributes(&unused, &multicastCopyKernel);
        hggcFuncGetAttributes(&unused, &ptrChasingKernel);
        hggcFuncGetAttributes(&unused, &multicastCopyKernel);
        hggcFuncGetAttributes(&unused, &memsetKernelDevice);
        hggcFuncGetAttributes(&unused, &memcmpKernelDevice);
        hggcFuncGetAttributes(&unused, &multicastMemcmpKernelDevice);
    }
}



