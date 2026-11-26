/* SPDX-FileCopyrightText: Copyright (c) 2026 T-Head (Shanghai) Semiconductor Co., Ltd. All rights reserved. */
/* SPDX-License-Identifier: Apache-2.0 */

#include <mpi.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    fputs("[mpi-probe] entering MPI_Init\n", stderr);
    fflush(stderr);
    if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
        return 1;
    }
    int rank, size;
    char host[256] = {0};
    if (MPI_Comm_rank(MPI_COMM_WORLD, &rank) != MPI_SUCCESS ||
        MPI_Comm_size(MPI_COMM_WORLD, &size) != MPI_SUCCESS ||
        size != 2 || gethostname(host, sizeof(host) - 1) != 0) {
        MPI_Abort(MPI_COMM_WORLD, 1);
        return 1;
    }
    fprintf(stderr, "[mpi-probe] rank %d initialized; entering barrier\n", rank);
    fflush(stderr);
    if (MPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS) {
        MPI_Abort(MPI_COMM_WORLD, 1);
        return 1;
    }
    printf("probe rank=%d host=%s\n", rank, host);
    fflush(stdout);
    return MPI_Finalize() != MPI_SUCCESS;
}
