/***************************************************************************************************
 * Copyright (c) 2023 - 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 **************************************************************************************************/
#include <CL/sycl.hpp>
#include <cute/tensor.hpp>
#include <syclcompat.hpp>

#include "utils.hpp"

using namespace cute;

using TileShape = Shape<_128, _128, _32>;

using TiledMma = TiledMMA<
        MMA_Atom<SM80_16x8x8_F32TF32TF32F32_TN>,
        Layout<Shape<_2,_2,_1>>, // 2x2x1 thread group
        Tile<_32,_32,_16>>;      // 32x32x16 MMA for LDSM, 1x2x1 value group

// Smem
using SmemLayoutAtomA = decltype(
composition(Swizzle<2,3,2>{},
            Layout<Shape <_32, _8>,
                    Stride< _1,_32>>{}));
using SmemCopyAtomA = Copy_Atom<UniversalCopy<tfloat32_t>, tfloat32_t>;
// Gmem
using GmemTiledCopyA = decltype(
make_tiled_copy(Copy_Atom<SM80_CP_ASYNC_CACHEGLOBAL<cute::uint128_t>, tfloat32_t>{},
                Layout<Shape <_16, _8>,
                        Stride< _1,_16>>{},
                Layout<Shape < _4, _1>>{}));

// Smem
using SmemLayoutAtomB = decltype(
composition(Swizzle<2,3,2>{},
            Layout<Shape <_32, _8>,
                    Stride< _1,_32>>{}));
using SmemCopyAtomB = Copy_Atom<UniversalCopy<tfloat32_t>, tfloat32_t>;
// Gmem
using GmemTiledCopyB = decltype(
make_tiled_copy(Copy_Atom<SM80_CP_ASYNC_CACHEGLOBAL<cute::uint128_t>, tfloat32_t>{},
                Layout<Shape <_16, _8>,
                        Stride< _1,_16>>{},
                Layout<Shape < _4, _1>>{}));

using Stages = Int<3>;

using SmemLayoutA = decltype(tile_to_shape(
        SmemLayoutAtomA{},
        make_shape(shape<0>(TileShape{}), shape<2>(TileShape{}), Stages{})));
using SmemLayoutB = decltype(tile_to_shape(
        SmemLayoutAtomB{},
        make_shape(shape<1>(TileShape{}), shape<2>(TileShape{}), Stages{})));

struct float2tf32 {
    CUTE_HOST_DEVICE
    tfloat32_t operator()(float&& arg) const {
      uint32_t storage = reinterpret_cast<uint32_t &>(arg);
      bool mantissa_bit = ((storage & (1 << 13)) != 0);
      bool round_bit = ((storage & (1 << 12)) != 0);
      bool sticky_bit = ((storage & ((1 << 12) - 1)) != 0);

      if ((round_bit && sticky_bit) || (round_bit && mantissa_bit)) {
        storage += uint32_t(1 << 13);
      }

      return tfloat32_t::bitcast(storage);
    }
};

template <class MShape, class NShape, class KShape,
          class TA, class AStride,
          class TB, class BStride,
          class TC, class CStride,
          class Alpha, class Beta>
CUTE_DEVICE
void
gemm_device(MShape M, NShape N, KShape K,
            TA const* A, AStride dA,
            TB const* B, BStride dB,
            TC      * C, CStride dC,
            Alpha alpha, Beta beta,
            char* smem)
{
  using namespace cute;
  using X = Underscore;

  // Shared memory buffers
  float* smemA = reinterpret_cast<float*>(smem);
  float* smemB = smemA + cosize_v<SmemLayoutA>;
  auto sA = make_tensor(make_smem_ptr(smemA), SmemLayoutA{});               // (BLK_M,BLK_K)
  auto sB = make_tensor(make_smem_ptr(smemB), SmemLayoutB{});               // (BLK_N,BLK_K)

  // Represent the full tensors
  auto mA = make_tensor(make_gmem_ptr(A), make_shape(M,K), dA);      // (M,K)
  auto mB = make_tensor(make_gmem_ptr(B), make_shape(N,K), dB);      // (N,K)
  auto mC = make_tensor(make_gmem_ptr(C), make_shape(M,N), dC);      // (M,N)

  // Get the appropriate blocks for this thread block --
  // potential for thread block locality
  auto blk_shape = TileShape{};// (BLK_M,BLK_N,BLK_K)
  // Compute m_coord, n_coord, and l_coord with their post-tiled shapes
  auto m_coord = idx2crd(int(syclcompat::work_group_id::x()), shape<0>(blk_shape));
  auto n_coord = idx2crd(int(syclcompat::work_group_id::y()), shape<1>(blk_shape));
  auto blk_coord = make_coord(m_coord, n_coord, _);

  auto gA = local_tile(mA, blk_shape, blk_coord, Step<_1, X,_1>{});  // (BLK_M,BLK_K,k)
  auto gB = local_tile(mB, blk_shape, blk_coord, Step< X,_1,_1>{});  // (BLK_N,BLK_K,k)
  auto gC = local_tile(mC, blk_shape, blk_coord, Step<_1,_1, X>{});  // (BLK_M,BLK_N)

  //
  // Partition the copying of A and B tiles across the threads
  //

  GmemTiledCopyA gmem_tiled_copy_A;
  GmemTiledCopyB gmem_tiled_copy_B;
  auto gmem_thr_copy_A = gmem_tiled_copy_A.get_slice(syclcompat::local_id::x());
  auto gmem_thr_copy_B = gmem_tiled_copy_B.get_slice(syclcompat::local_id::x());

  Tensor tAgA = gmem_thr_copy_A.partition_S(gA);                             // (ACPY,ACPY_M,ACPY_K,k)
  Tensor tAsA = gmem_thr_copy_A.partition_D(sA);                             // (ACPY,ACPY_M,ACPY_K,PIPE)
  Tensor tBgB = gmem_thr_copy_B.partition_S(gB);                             // (BCPY,BCPY_N,BCPY_K,k)
  Tensor tBsB = gmem_thr_copy_B.partition_D(sB);                             // (BCPY,BCPY_N,BCPY_K,PIPE)

  auto k_tile_iter  = make_coord_iterator(shape<2>(gA));
  int  k_tile_count = size<2>(gA);

  for (int k_pipe = 0; k_pipe < Stages{}-1; ++k_pipe) {
    copy(gmem_tiled_copy_A, tAgA(_,_,_,*k_tile_iter), tAsA(_,_,_,k_pipe));
    copy(gmem_tiled_copy_B, tBgB(_,_,_,*k_tile_iter), tBsB(_,_,_,k_pipe));
    cp_async_fence();
    ++k_tile_iter;
    --k_tile_count;
  }

  //
  // Define C accumulators and A/B partitioning
  //

  TiledMma tiled_mma;
  auto thr_mma = tiled_mma.get_thread_slice(syclcompat::local_id::x());
  Tensor tCrA  = thr_mma.partition_fragment_A(sA(_,_,0));                    // (MMA,MMA_M,MMA_K)
  Tensor tCrB  = thr_mma.partition_fragment_B(sB(_,_,0));                    // (MMA,MMA_N,MMA_K)
  Tensor tCgC = thr_mma.partition_C(gC);

  auto smem_tiled_copy_A   = make_tiled_copy_A(SmemCopyAtomA{}, tiled_mma);
  auto smem_thr_copy_A     = smem_tiled_copy_A.get_thread_slice(syclcompat::local_id::x());
  Tensor tCsA           = smem_thr_copy_A.partition_S(sA);                   // (CPY,CPY_M,CPY_K,PIPE)
  Tensor tCrA_copy_view = smem_thr_copy_A.retile_D(tCrA);
  CUTE_STATIC_ASSERT_V(size<1>(tCsA) == size<1>(tCrA_copy_view));            // CPY_M
  CUTE_STATIC_ASSERT_V(size<2>(tCsA) == size<2>(tCrA_copy_view));            // CPY_K

  auto smem_tiled_copy_B = make_tiled_copy_B(SmemCopyAtomB{}, tiled_mma);
  auto smem_thr_copy_B   = smem_tiled_copy_B.get_thread_slice(syclcompat::local_id::x());
  Tensor tCsB              = smem_thr_copy_B.partition_S(sB);                // (CPY,CPY_N,CPY_K,PIPE)
  Tensor tCrB_copy_view    = smem_thr_copy_B.retile_D(tCrB);
  CUTE_STATIC_ASSERT_V(size<1>(tCsB) == size<1>(tCrB_copy_view));            // CPY_N
  CUTE_STATIC_ASSERT_V(size<2>(tCsB) == size<2>(tCrB_copy_view));            // CPY_K

  // Allocate the accumulators -- same size as the projected data
  Tensor tCrC = partition_fragment_C(tiled_mma, take<0,2>(blk_shape)); // (MMA,MMA_M,MMA_N)

  // Clear the accumulators
  clear(tCrC);

  // Current pipe index in smem to read from
  int smem_pipe_read  = 0;
  // Current pipe index in smem to write to
  int smem_pipe_write = Stages{}-1;

  Tensor tCsA_p = tCsA(_,_,_,smem_pipe_read);
  Tensor tCsB_p = tCsB(_,_,_,smem_pipe_read);

  // Size of the register pipeline
  auto K_BLOCK_MAX = size<2>(tCrA);

  // PREFETCH register pipeline
  if (K_BLOCK_MAX > 1) {
    // Wait until our first prefetched tile is loaded in
    cp_async_wait<Stages{}-2>();
    syclcompat::wg_barrier();

    // Prefetch the first rmem from the first k-tile
    copy(smem_tiled_copy_A, tCsA_p(_,_,Int<0>{}), tCrA_copy_view(_,_,Int<0>{}));
    copy(smem_tiled_copy_B, tCsB_p(_,_,Int<0>{}), tCrB_copy_view(_,_,Int<0>{}));
  }

  CUTE_NO_UNROLL
  for ( ; k_tile_count > -(Stages{}-1); --k_tile_count)
  {
    // Pipeline the outer products with a static for loop.
    //
    // Note, the for_each() function is required here to ensure `k_block` is of type Int<x>.
    CUTE_UNROLL
    for (auto k_block = 0; k_block < K_BLOCK_MAX; ++k_block) {
      if (k_block == K_BLOCK_MAX - 1) {
        // Slice the smem_pipe_read smem
        tCsA_p = tCsA(_, _, _, smem_pipe_read);
        tCsB_p = tCsB(_, _, _, smem_pipe_read);

        // Commit the smem for smem_pipe_read
        cp_async_wait<Stages{} - 2>();
        syclcompat::wg_barrier();
      }

      // Load A, B shmem->regs for k_block+1
      auto k_block_next = (k_block + Int<1>{}) % K_BLOCK_MAX;  // static
      copy(smem_tiled_copy_A, tCsA_p(_, _, k_block_next), tCrA_copy_view(_, _, k_block_next));
      copy(smem_tiled_copy_B, tCsB_p(_, _, k_block_next), tCrB_copy_view(_, _, k_block_next));
      // Copy gmem to smem before computing gemm on each k-pipe
      if (k_block == 0) {
        if (k_tile_count > 0) {
          copy(gmem_tiled_copy_A, tAgA(_, _, _, *k_tile_iter), tAsA(_, _, _, smem_pipe_write));
          copy(gmem_tiled_copy_B, tBgB(_, _, _, *k_tile_iter), tBsB(_, _, _, smem_pipe_write));
          cp_async_fence();
          ++k_tile_iter;
        }
        // Advance the pipe -- Doing it here accounts for K_BLOCK_MAX = 1 (no rmem pipe)
        smem_pipe_write = smem_pipe_read;
        ++smem_pipe_read;
        smem_pipe_read = (smem_pipe_read == Stages{}) ? 0 : smem_pipe_read;
      }

      // Transform before compute
      cute::transform(tCrA(_, _, k_block), identity());
      cute::transform(tCrB(_, _, k_block), identity());
      // Thread-level register gemm for k_block
      cute::gemm(tiled_mma, tCrC, tCrA(_, _, k_block), tCrB(_, _, k_block), tCrC);
    }
  }

  //
  // Epilogue
  //
  // Represent the full output tensor
  Tensor mC_mnl = make_tensor(make_gmem_ptr(C), make_shape(M,N), dC);                 // (m,n,l)
  Tensor mD_mnl = make_tensor(make_gmem_ptr(C), make_shape(M,N), dC);                 // (m,n,l)
  Tensor gC_mnl = local_tile(mC_mnl, blk_shape, make_coord(_,_,_), Step<_1,_1, X>{});    // (BLK_M,BLK_N,m,n,l)
  Tensor gD_mnl = local_tile(mD_mnl, blk_shape, make_coord(_,_,_), Step<_1,_1, X>{});    // (BLK_M,BLK_N,m,n,l)

  // Slice to get the tile this CTA is responsible for
  Tensor gD = gD_mnl(_,_,m_coord,n_coord);                                                 // (BLK_M,BLK_N)

  // Partition source and destination tiles to match the accumulator partitioning
  Tensor tCgD = thr_mma.partition_C(gD);                                       // (VEC,THR_M,THR_N)

  // Make an identity coordinate tensor for predicating our output MN tile
  auto cD = make_identity_tensor(make_shape(unwrap(shape<0>(gD)), unwrap(shape<1>(gD))));
  Tensor tCcD = thr_mma.partition_C(cD);

  // source is not needed, avoid load
  CUTE_UNROLL
  for (int i = 0; i < size(tCrC); ++i) {
    tCgD(i) = tCrC(i);
  }
}


template <typename TA, typename TB, typename TC,
          typename Alpha, typename Beta>
void
gemm(int m, int n, int k,
     Alpha alpha,
     TA const* A, int ldA,
     TB const* B, int ldB,
     Beta beta,
     TC      * C, int ldC)
{
  using namespace cute;

  // Define shapes (dynamic)
  auto M = int(m);
  auto N = int(n);
  auto K = int(k);

  // Define strides (mixed)
  auto dA = make_stride(Int<1>{}, ldA);
  auto dB = make_stride(Int<1>{}, ldB);
  auto dC = make_stride(Int<1>{}, ldC);

  // Define block sizes (static)
  auto bM = Int<128>{};
  auto bN = Int<128>{};
  auto bK = Int< 32>{};

  // Define the block layouts (static)
  auto sC = make_layout(make_shape(bM,bN));

  auto sA = tile_to_shape(SmemLayoutAtomA{}, make_shape(bM,bK));
  auto sB = tile_to_shape(SmemLayoutAtomB{}, make_shape(bN,bK));

  const auto block = syclcompat::dim3(128);
  const auto grid = syclcompat::dim3(ceil_div(size(M), size(bM)),
                                     ceil_div(size(N), size(bN)));

  const int smem_size = (cosize_v<SmemLayoutA> + cosize_v<SmemLayoutA>) * sizeof(tfloat32_t);

  utils::launch_kernel<gemm_device<int, int, int,
          TA, decltype(dA),
          TB, decltype(dB),
          TC, decltype(dC),
          Alpha, Beta>>
          (grid, block, smem_size, M,  N,  K,
           A, dA,
           B, dB,
           C, dC,
           alpha, beta);
}

int main(int argc, char** argv)
{
  int m = 5120;
  if (argc >= 2)
    sscanf(argv[1], "%d", &m);

  int n = 5120;
  if (argc >= 3)
    sscanf(argv[2], "%d", &n);

  int k = 4096;
  if (argc >= 4)
    sscanf(argv[3], "%d", &k);

  using TA = float;
  using TB = float;
  using TC = float;
  using TI = float;

  // This MMA operation casts the input matrices to TF32 type, causing precision issues
  // when comparing with cublas output. Cast the input matrices to TF32 to avoid
  // these issues.
  using TAT = tfloat32_t;
  using TBT = tfloat32_t;

  utils::test_gemm<gemm<TA, TB, TC, TI, TI>, TA, TB, TC, TI, TAT, TBT>(m, n, k);

  return 0;
}
