/*
 * Copyright (c) 2023, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cuvs/neighbors/detail/refine_common.hpp>
#include <raft/core/host_mdspan.hpp>
#include <raft/core/nvtx.hpp>

#include <algorithm>
#include <omp.h>

namespace cuvs::neighbors::detail {

template <typename DC, typename IdxT, typename DataT, typename DistanceT, typename ExtentsT>
[[gnu::optimize(3), gnu::optimize("tree-vectorize")]] void refine_host_impl(
  raft::host_matrix_view<const DataT, ExtentsT, raft::row_major> dataset,
  raft::host_matrix_view<const DataT, ExtentsT, raft::row_major> queries,
  raft::host_matrix_view<const IdxT, ExtentsT, raft::row_major> neighbor_candidates,
  raft::host_matrix_view<IdxT, ExtentsT, raft::row_major> indices,
  raft::host_matrix_view<DistanceT, ExtentsT, raft::row_major> distances)
{
  size_t n_queries = queries.extent(0);
  size_t n_rows    = dataset.extent(0);
  size_t dim       = dataset.extent(1);
  size_t orig_k    = neighbor_candidates.extent(1);
  size_t refined_k = indices.extent(1);

  common::nvtx::range<common::nvtx::domain::raft> fun_scope(
    "neighbors::refine_host(%zu, %zu -> %zu)", n_queries, orig_k, refined_k);

  auto suggested_n_threads = std::max(1, std::min(omp_get_num_procs(), omp_get_max_threads()));
  if (size_t(suggested_n_threads) > n_queries) { suggested_n_threads = n_queries; }

#pragma omp parallel num_threads(suggested_n_threads)
  {
    std::vector<std::tuple<DistanceT, IdxT>> refined_pairs(orig_k);
    for (size_t i = omp_get_thread_num(); i < n_queries; i += omp_get_num_threads()) {
      // Compute the refined distance using original dataset vectors
      const DataT* query = queries.data_handle() + dim * i;
      for (size_t j = 0; j < orig_k; j++) {
        IdxT id            = neighbor_candidates(i, j);
        DistanceT distance = 0.0;
        if (static_cast<size_t>(id) >= n_rows) {
          distance = std::numeric_limits<DistanceT>::max();
        } else {
          const DataT* row = dataset.data_handle() + dim * id;
          for (size_t k = 0; k < dim; k++) {
            distance += DC::template eval<DistanceT>(query[k], row[k]);
          }
        }
        refined_pairs[j] = std::make_tuple(distance, id);
      }
      // Sort the query neighbors by their refined distances
      std::sort(refined_pairs.begin(), refined_pairs.end());
      // Store first refined_k neighbors
      for (size_t j = 0; j < refined_k; j++) {
        indices(i, j) = std::get<1>(refined_pairs[j]);
        if (distances.data_handle() != nullptr) {
          distances(i, j) = DC::template postprocess(std::get<0>(refined_pairs[j]));
        }
      }
    }
  }
}

struct distance_comp_l2 {
  template <typename DistanceT>
  static inline auto eval(const DistanceT& a, const DistanceT& b) -> DistanceT
  {
    auto d = a - b;
    return d * d;
  }
  template <typename DistanceT>
  static inline auto postprocess(const DistanceT& a) -> DistanceT
  {
    return a;
  }
};

struct distance_comp_inner {
  template <typename DistanceT>
  static inline auto eval(const DistanceT& a, const DistanceT& b) -> DistanceT
  {
    return -a * b;
  }
  template <typename DistanceT>
  static inline auto postprocess(const DistanceT& a) -> DistanceT
  {
    return -a;
  }
};

/**
 * Naive CPU implementation of refine operation
 *
 * All pointers are expected to be accessible on the host.
 */
template <typename IdxT, typename DataT, typename DistanceT, typename ExtentsT>
[[gnu::optimize(3), gnu::optimize("tree-vectorize")]] void refine_host(
  raft::host_matrix_view<const DataT, ExtentsT, raft::row_major> dataset,
  raft::host_matrix_view<const DataT, ExtentsT, raft::row_major> queries,
  raft::host_matrix_view<const IdxT, ExtentsT, raft::row_major> neighbor_candidates,
  raft::host_matrix_view<IdxT, ExtentsT, raft::row_major> indices,
  raft::host_matrix_view<DistanceT, ExtentsT, raft::row_major> distances,
  distance::DistanceType metric = distance::DistanceType::L2Unexpanded)
{
  refine_check_input(dataset.extents(),
                     queries.extents(),
                     neighbor_candidates.extents(),
                     indices.extents(),
                     distances.extents(),
                     metric);

  switch (metric) {
    case cuvs::distance::DistanceType::L2Expanded:
      return refine_host_impl<distance_comp_l2>(
        dataset, queries, neighbor_candidates, indices, distances);
    case cuvs::distance::DistanceType::InnerProduct:
      return refine_host_impl<distance_comp_inner>(
        dataset, queries, neighbor_candidates, indices, distances);
    default: throw raft::logic_error("Unsupported metric");
  }
}

}  // namespace cuvs::neighbors::detail
