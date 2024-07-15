/***************************************************************************************************
 * Copyright (c) 2024 - 2024 Codeplay Software Ltd. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *this list of conditions and the following disclaimer.
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
#pragma once

#include "cutlass/cutlass.h"
#include "cutlass/gemm/dispatch_policy.hpp"

#include "cute/algorithm/functional.hpp"
#include "cute/algorithm/gemm.hpp"
#include "cute/atom/mma_atom.hpp"
#include "cute/tensor_predicate.hpp"

/////////////////////////////////////////////////////////////////////////////////////////////////
namespace cutlass::gemm::collective {
using namespace cute;
/////////////////////////////////////////////////////////////////////////////////////////////////
#define get_sub_group_id()                                                     \
  (sycl::ext::oneapi::experimental::this_nd_item<3>()                          \
       .get_sub_group()                                                        \
       .get_group_id()[0])

template <
  class TileShape_,
  class ElementA_,
  class StrideA_,
  class ElementB_,
  class StrideB_,
  class TiledMma_,
  class GmemTiledCopyA_,
  class SmemLayoutAtomA_,
  class SmemCopyAtomA_,
  class TransformA_,
  class GmemTiledCopyB_,
  class SmemLayoutAtomB_,
  class SmemCopyAtomB_,
  class TransformB_>
struct CollectiveMma<
    MainloopIntelPVCUnpredicated,
    TileShape_,
    ElementA_,
    StrideA_,
    ElementB_,
    StrideB_,
    TiledMma_,
    GmemTiledCopyA_,
    SmemLayoutAtomA_,
    SmemCopyAtomA_,
    TransformA_,
    GmemTiledCopyB_,
    SmemLayoutAtomB_,
    SmemCopyAtomB_,
    TransformB_>
{
  //
  // Type Aliases
  //
  using DispatchPolicy = MainloopIntelPVCUnpredicated;
  using TileShape = TileShape_;
  using ElementA = ElementA_;
  using StrideA = StrideA_;
  using ElementB = ElementB_;
  using StrideB = StrideB_;
  using TiledMma = TiledMma_;
  using ElementAccumulator = typename TiledMma::ValTypeC;
  using GmemTiledCopyA = GmemTiledCopyA_;
  using GmemTiledCopyB = GmemTiledCopyB_;
  using SmemLayoutAtomA = SmemLayoutAtomA_;
  using SmemLayoutAtomB = SmemLayoutAtomB_;
  using SmemCopyAtomA = SmemCopyAtomA_;
  using SmemCopyAtomB = SmemCopyAtomB_;
  using TransformA = TransformA_;
  using TransformB = TransformB_;
  using ArchTag = typename DispatchPolicy::ArchTag;

  TileShape tile_shape;
  static constexpr auto wg_tile_m = decltype(get<0>(tile_shape))::value;
  static constexpr auto wg_tile_n = decltype(get<1>(tile_shape))::value;
  static constexpr auto sg_tile_m = decltype(get<2>(tile_shape))::value;
  static constexpr auto sg_tile_n = decltype(get<3>(tile_shape))::value;
  static constexpr auto sg_tile_k = decltype(get<4>(tile_shape))::value;
  static constexpr auto sg_per_wg_m = wg_tile_m / sg_tile_m;
  static constexpr auto sg_per_wg_n = wg_tile_n / sg_tile_n;
  static constexpr int SubgroupSize = DispatchPolicy::SubgroupSize;

  static constexpr int DpasM = get<0>(
      shape(typename TiledMma::LayoutA_TV{})); // rows per dpas operation per
                                               // sub_group for Matrix A
  static constexpr int DpasN = get<1>(
      shape(typename TiledMma::LayoutB_TV{})); // cols per dpas operation per
                                               // sub_group for Matrix B
  static constexpr int DpasK = get<1>(
      shape(typename TiledMma::LayoutA_TV{})); // cols per dpas operation per
                                               // sub_group for Matrix A

  static constexpr uint32_t MaxThreadsPerBlock = DpasM * DpasN;
  static constexpr uint32_t MinBlocksPerMultiprocessor = 1;

  static constexpr int FragsM = sg_tile_m / DpasM; // A frags per sub_group
  static constexpr int FragsN = sg_tile_n / DpasN; // B frags per sub_group
  static constexpr int FragsK = sg_tile_k / DpasK;

  // Calculate the vector width based on the amount of registers
  // required per work item by dividing the total fragment size by
  // the sub_group size.
  static constexpr int VecC = (DpasN * DpasM) / SubgroupSize;
  static constexpr int VecA = (DpasM * DpasK) / SubgroupSize;
  static constexpr int VecB = (DpasN * DpasK) / SubgroupSize;

  // Host side kernel arguments
  struct Arguments {
    ElementA const* ptr_A;
    StrideA dA;
    ElementB const* ptr_B;
    StrideB dB;
  };

  struct Params {
    using XE_Copy_A = decltype(make_xe_2d_copy<GmemTiledCopyA>(make_tensor(static_cast<ElementA const*>(nullptr), 
                                repeat_like(StrideA{}, int32_t(0)), StrideA{})));
    using XE_Copy_B = decltype(make_xe_2d_copy<GmemTiledCopyB>(make_tensor(static_cast<ElementB const*>(nullptr), 
                                repeat_like(StrideB{}, int32_t(0)), StrideB{})));
    XE_Copy_A gmem_tiled_copy_a;
    XE_Copy_B gmem_tiled_copy_b;
  };

  //
  // Methods
  //

  CollectiveMma() = default;

  template <class ProblemShape>
  static constexpr Params
  to_underlying_arguments(ProblemShape const& problem_shape, Arguments const& args, void* workspace) {
    (void) workspace;

    auto problem_shape_MNKL = append<4>(problem_shape, 1);
    auto [M,N,K,L] = problem_shape_MNKL;

    Tensor tensorA = make_tensor(args.ptr_A, make_layout(make_shape(M,K,L), args.dA));
    Tensor tensorB = make_tensor(args.ptr_B, make_layout(make_shape(K,N,L), args.dB));

    typename Params::XE_Copy_A copyA = make_xe_2d_copy<GmemTiledCopyA>(tensorA);
    typename Params::XE_Copy_B copyB = make_xe_2d_copy<GmemTiledCopyB>(tensorB);
    return Params{copyA, copyB};
  }

  /// Perform a subgroup-scoped matrix multiply-accumulate
  template <
    class FrgTensorD,
    class TensorA,
    class TensorB,
    class FrgTensorC,
    class KTileIterator,
    class ResidueMNK
  >
  CUTLASS_DEVICE void
  operator() (
      FrgTensorD &accum,
      TensorA gA,
      TensorB gB,
      FrgTensorC const &src_accum,
      KTileIterator k_tile_iter, int k_tile_count,
      ResidueMNK residue_mnk,
      int thread_idx,
      char *smem_buf,
      Params const& mainloop) 
  {
    (void)residue_mnk;
    (void)thread_idx;
    (void)smem_buf;

    static_assert(is_rmem<FrgTensorD>::value, "D tensor must be rmem resident.");
    static_assert(is_tuple<typename TensorA::engine_type::iterator::value_type>::value, "A tensor must be a tuple iterator.");
    static_assert(is_tuple<typename TensorB::engine_type::iterator::value_type>::value, "B tensor must be a tuple iterator.");
    static_assert(is_rmem<FrgTensorC>::value, "C tensor must be rmem resident.");

    // Tensor to hold input data
    Tensor tAr = make_tensor<typename TiledMma::ValTypeA>(
        Shape<Int<sg_tile_m * FragsK>, Int<1>>{});

    constexpr int version =
        is_same_v<GmemTiledCopyB, XE_2D_U16x16x16x2x1_V> ? 1 : 2;
    Tensor tBr = make_tensor<typename TiledMma::ValTypeB>(
        Shape<Int<sg_tile_k * version>, Int<FragsN / version>>{});

    Tensor tAr_view = make_tensor(static_cast<decltype(tAr) &&>(tAr).data(),
                            Shape<Int<VecA>, Int<FragsM>, Int<FragsK>>{});
    Tensor tBr_view = make_tensor(static_cast<decltype(tBr) &&>(tBr).data(),
                                  Shape<Int<VecB>, Int<FragsK>, Int<FragsN>>{});

    // Instantiate the M MA object
    TiledMma tiled_mma;

    int K = size<1>(mainloop.gmem_tiled_copy_a.tensor);

    /* Cooperative prefetch
       Divice the thread space to sg_per_wg_m x sg_per_wg_n, all the threads in one row/col use the same tile A/B. 
       Each thread loads sizeof(tile A or B) / numof(sg_per_wg_n or sg_per_wg_m). 
       
       Currently, sg_per_wg_m x sg_per_wg_n = 4 x 8 is the most efficient
    */
    // TODO: Replace the demo cooperative prefetch with more general way.
    Tensor tAi = make_tensor(
        make_inttuple_iter(
            *gA.data() +
            make_coord((get_sub_group_id() % sg_per_wg_n % 4) * DpasM, 0)),
        make_layout(make_shape(_1{}, _1{}, K),
                    make_stride(_1{}, E<0>{}, E<1>{})));
    Tensor tBi = make_tensor(
        make_inttuple_iter(
            *gB.data() +
            make_coord((get_sub_group_id() / sg_per_wg_n / 2 % 2) * DpasK,
                       (get_sub_group_id() / sg_per_wg_n % 2 * 2) * DpasN)),
        make_layout(make_shape(_1{}, K, _1{}),
                    make_stride(_1{}, E<0>{}, E<1>{})));
    //
    // Mainloop
    //
    int prefetch_k = 0;
    for (int i = 0; i < 3; i++) {
      prefetch(mainloop.gmem_tiled_copy_a, tAi(_, _, prefetch_k));
      prefetch(mainloop.gmem_tiled_copy_b, tBi(_, prefetch_k, _));
      prefetch_k += sg_tile_k;
    }

    for (int k_tile = 0, k = 0; k_tile < k_tile_count;
         ++k_tile, k += DpasK * FragsK) {
      // Copy gmem to rmem for the first k_tile
      copy(mainloop.gmem_tiled_copy_a, gA(_, _, k), tAr);
      copy(mainloop.gmem_tiled_copy_b, gB(_, k, _), tBr);

      prefetch(mainloop.gmem_tiled_copy_a, tAi(_, _, prefetch_k));
      prefetch(mainloop.gmem_tiled_copy_b, tBi(_, prefetch_k, _));
      prefetch_k += sg_tile_k;

      for (int kl = 0; kl < FragsK; kl++) {
        cute::gemm(tiled_mma, accum, tAr_view(_, _, kl), tBr_view(_, kl, _),
                   src_accum);
      }
    }
  }
};

} // namespace cutlass::gemm::collective

/////////////////////////////////////////////////////////////////////////////////////////////////
