#include <ATen/ATen.h>
#include <ATen/record_function.h>

#include <core/Memory.h>
#include <core/detail/TensorInfo.h>
#include <oneDNN/oneDNN.h>
#include <runtime/Utils.h>
#include <utils/DPCPP.h>
#include "Loops.h"
#include "comm/RegistrationDeclarations.h"

//#include <aten/operators/MemoryAccess.h>
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/ApplyUtils.h"
#include "comm/Numerics.h"
#include "comm/SimpleReduce.h"
#include "utils/ComputeEngine.h"
#include "utils/CustomOperatorRegistration.h"
/*
softmax forward and backward follow the same optimization routine, we take
forward as an example here. softmax = exp(x) / sum(exp(x)) to ensuare the exp(x)
in range of [0, 1], we use exp(x - max) to replace exp(x) then softmax = exp(x -
max) / sum(exp(x - max)) Any input tensor for softmax can be viewed as
[outer_size, dim_size, inner_size] If the softmax axis is the last dim (dim=-1),
then the inner_size = 1, and the input tensor can be viewed as [outer_size,
dim_size, 1] If the somftmax axis is not the last dim (dim!=-1), then the input
tensor can be viewed as [outer_size, dim_size, inner_size] Genearally, three
steps are needed to get the softmax result
1. read data and get the max value
2. read data and get the sum value
3. read data and compute element-wise result


***************************************************************************************
dispatch_softmax_forward_kernel is the fast path for softmax forward with
inner_size=1, by reading the input elements only once and keep them in the
registers. When MaxWorkGroupSize (1024 on PVC and ATSM) * INNER_LOOP >=
dim_size, this fast path can be selected

The main steps includes:
1. each workitem load INNER_LOOP [NUM][vec_size] numbers of elements
2. Get max/sum value along dim_size
   if dim_size < 16 and dim_size * sizeof(scalar_t) < sizeof(float16), reduce
happened internal one workitem, otherwise reduced happened internal one subgroup
or group and will be processed by group_reduce function.
3. compute and store the softmax result into the global memory

Configs:
   The vec_size is decided by datatype and dim_size:
   double && dim_size % 2 == 0: vec_size = 2 (sizeof(float4)/sizeof(double))
   float  && dim_size % 4 == 0: vec_size = 4 (sizeof(float4)/sizeof(float))
   bf16/fp16 && dim_size % 8 == 0: vec_size = 8
(sizeof(float4)/sizeof(bf16/fp16)) otherwise, vec_size = 1

   Initial INNER_LOOP = sizeof(float8) / sizeof(scalar_t)
   if dim_size < INNER_LOOP * SIMD16,
      INNER_LOOP = sizeof(float8) / sizeof(scalar_t) * 2
      SIMD=16
   otherwise,
      INNER_LOOP = sizeof(float8) / sizeof(scalar_t)
      SIMD=32

   WorkGroupSize equals to multi times of SIMD covering dim_size / INNER_LOOP
   WorkGroupNum equals to outer_size
   If WorkGroupNum is too large and WorkGroupSize is small, we will enlarge
WorkGroupSize to process multiple dim_size elements.


***************************************************************************************
softmax_forward_kernel is the reference path for softmax forward with
inner_size=1 input data cannot be reused and must be loaded in each step
including: get max, get sum, update result

   Configs:
   double: vec_size = 2 (sizeof(float4)/sizeof(double))
   float: vec_size = 4 (sizeof(float4)/sizeof(float))
   bf16/fp16: vec_size = 8 (sizeof(float4)/sizeof(bf16/fp16))
   The non-alignment will be handled in this kernel and max_vec_size will always
be selected.

   WorkGroupSize equals the MaxWorkGroupSize
   WorkGroupNum equals to outer_size


***************************************************************************************
spatial_softmax_forward used for softmax forward with inner_size != 1
   input tensor [outer_size, dim_size, inner_size]
   workitem space [outer_size] [DIM_NUM][dim_size/DIM_NUM]
[INNER_NUM][inner_size/INNER_NUM]
*/

using namespace dnnl;
using namespace xpu::dpcpp::detail;
using namespace xpu::dpcpp;
using namespace xpu::oneDNN;

#define MIN_WG_NUM 32768

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

template <
    int SIMD,
    typename accscalar_t,
    typename reduce_op,
    typename nd_item_id,
    typename local_shared>
static inline void group_reduce(
    nd_item_id item_id,
    int lid_row,
    int sub_group_num,
    accscalar_t& val,
    accscalar_t init,
    const local_shared& local_data,
    reduce_op bin_op) {
  auto sg = item_id.get_sub_group();

  // dynamic get SIMD width result in big performance drop
  // uint32_t SIMD = sg.get_local_range()[0];
#pragma unroll
  for (int i = 1; i < SIMD; i <<= 1) {
    val = bin_op(val, static_cast<accscalar_t>(sycl::shift_group_left(sg, val, i)));
  }
  if (sub_group_num == 1) {
    val = sycl::group_broadcast(sg, val, 0);
    return;
  }
  uint32_t sg_local_id = sg.get_local_linear_id();
  uint32_t sg_id = sg.get_group_linear_id();
  // reduce internal each subgroup, each subgroup will generate one result
  // there are WGroupSize/subGroupSize elements after this step
  int idx = sg_id - (lid_row * sub_group_num);
  if (sg_local_id == 0) {
    local_data[lid_row][idx] = val;
  }
  item_id.barrier(dpcpp_local_fence);

  // use one subgroup to reduce WGroupSize/subGroupSize elements
  // into the final result
  if (idx == 0) {
    val = init;
    if (sg_local_id < sub_group_num) {
      val = accscalar_t(local_data[lid_row][sg_local_id]);
    }
    for (int i = sg_local_id + SIMD; i < sub_group_num; i += SIMD) {
      val = bin_op(val, static_cast<accscalar_t>(local_data[lid_row][i]));
    }
#pragma unroll
    for (int i = 1; i < SIMD; i <<= 1) {
      val = bin_op(val, static_cast<accscalar_t>(sycl::shift_group_left(sg, val, i)));
      if (i >= ((sub_group_num + 1) >> 1))
        break;
    }

    // the 0th WI (the 0th WI in the 0th sub_group) generate the final result
    if (sg_local_id == 0) {
      local_data[lid_row][0] = val;
    }
  }

  item_id.barrier(dpcpp_local_fence);
  val = local_data[lid_row][0];
} // namespace impl

template <
    int vec_size,
    typename accscalar_t,
    typename reduce_op,
    typename nd_item_id,
    typename local_shared>
static inline void group_reduce_spatial(
    nd_item_id item_id,
    accscalar_t input[vec_size],
    const local_shared& local_data,
    int block_row,
    reduce_op bin_op) {
  auto local_row_id = item_id.get_local_id(1);
  auto local_col_id = item_id.get_local_id(2);

#pragma unroll(vec_size)
  for (int j = 0; j < vec_size; ++j) {
    local_data[local_row_id][local_col_id][j] = input[j];
  }
  item_id.barrier(dpcpp_local_fence);

  int k = 1;
  while (k < block_row) {
    if (local_row_id % (k << 1) == 0 && local_row_id + k < block_row)
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        local_data[local_row_id][local_col_id][j] = bin_op(
            local_data[local_row_id][local_col_id][j],
            local_data[local_row_id + k][local_col_id][j]);
      }
    k *= 2;
    item_id.barrier(dpcpp_local_fence);
  }
}

template <int SIMD, int vec_size, int NUM>
static inline void get_wgroup_size(
    uint64_t dim_size,
    int outer_size,
    int& sub_group_num,
    int& range,
    int& global_size_row,
    int& local_size_row,
    int& local_size_col) {
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  int maxWGSize = dpcppMaxWorkGroupSize(dev_id);

  int local_size = (dim_size + NUM * vec_size - 1) / (NUM * vec_size);
  local_size = std::min(local_size, maxWGSize);
  // select the local_size_col to cover the dim_size
  sub_group_num = (local_size + SIMD - 1) / SIMD;
  local_size_col = sub_group_num * SIMD;
  // if one workitem [NUM][vec_size] can cover the dim_size number of elements
  // local_size_col will be 1
  if (dim_size <= vec_size * NUM) {
    local_size_col = 1;
    local_size_row = SIMD;
    global_size_row = (outer_size + local_size_row - 1) / local_size_row;
    return;
  }

  // if outer_size is too large and local_size_col is small,
  // then use one workgroup to handle multi rows (dim_size)
  local_size_row = 1;
  global_size_row = outer_size;
  while ((global_size_row >> 1) > MIN_WG_NUM &&
         (local_size_row << 1) * local_size_col <= maxWGSize &&
         !(global_size_row % 2)) {
    global_size_row = global_size_row >> 1;
    local_size_row = local_size_row << 1;
  }

  // compute the reduce range
  range = SIMD;
  while (sub_group_num <= (range >> 1)) {
    range = range >> 1;
  }
}

// this method help to divide the computation resource for spatial_softmax
template <int vec_size>
static inline void get_wgroup_size_spatial(
    int bs,
    int dim_size,
    int inner_size,
    int& GroupSize,
    int& GroupRow) {
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  int maxWGSize = dpcppMaxWorkGroupSize(dev_id);
  int total_resource = dpcppMaxWorkItemsPerTile(dev_id);

  // set the GroupSize smaller to ensure larger group number
  // smaller GroupSize is friendly to the tail case
  GroupSize = int((inner_size + vec_size - 1) / vec_size);
  GroupSize = std::min(GroupSize, SIMD32);
  auto local_group_num = (inner_size + GroupSize - 1) / GroupSize;

  // enlarge the GroupRow to occupy all the computation resource
  GroupRow = 1;
  while (bs * GroupRow * local_group_num * GroupSize <
         total_resource * vec_size) {
    GroupRow = GroupRow << 1;
    if (GroupRow * SIMD32 == maxWGSize)
      break;
  }
  GroupRow = std::min(GroupRow, int(dim_size));
}

template <
    int INNER_LOOP,
    int vec_size,
    int SIMD,
    typename scalar_t,
    typename accscalar_t,
    typename IndexType,
    bool LogSoftMax,
    int outer_loop,
    bool is_masked,
    typename calc_t,
    typename vec_t>
struct DispatchSoftmaxForwardKernelFunctor {
  [[intel::reqd_sub_group_size(SIMD)]] void operator()(
      sycl::nd_item<1> item_id) const {
    if (local_size == 1 && item_id.get_global_id(0) >= outer_size)
      return;

    uint32_t lid_row = 0;
    uint32_t lid_col = item_id.get_local_id(0);
    uint32_t group_offset = item_id.get_group(0) * dim_size;
    if (local_size_row != 1) {
      lid_row = item_id.get_local_id(0) / local_size;
      lid_col = item_id.get_local_id(0) % local_size;
      group_offset =
          (item_id.get_group(0) * local_size_row + lid_row) * dim_size;
    }
    vec_t reg_in[outer_loop];
    vec_t reg_mask[outer_loop];
    auto lid_offset = lid_col * vec_size;
    auto local_stride = local_size * vec_size;

    // load data and get max value
    accscalar_t max_value = std::numeric_limits<accscalar_t>::lowest();
#pragma unroll(outer_loop)
    for (int i = 0; i < outer_loop; ++i) {
      auto index = i * local_stride + lid_offset;
      if (index >= dim_size)
        break;

      reg_in[i] = *(reinterpret_cast<vec_t*>(in_data + group_offset + index));
      if constexpr (is_masked) {
        auto vec_offset = group_offset + index;
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j) {
          auto linear_idx = vec_offset + j;
          auto mask_offset = input_calc.get(linear_idx)[1];
          reg_mask[i][j] = mask_data[mask_offset];
        }
      }
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        if constexpr (is_masked) {
          if (reg_mask[i][j]) {
            reg_in[i][j] = neginf;
          }
        }
        max_value =
            Numerics<accscalar_t>::max(max_value, accscalar_t(reg_in[i][j]));
      }
    }
    if (local_size > 1) {
      group_reduce<SIMD, accscalar_t>(
          item_id,
          lid_row,
          sub_group_num,
          max_value,
          std::numeric_limits<accscalar_t>::lowest(),
          local_max,
          [](accscalar_t a, accscalar_t b) {
            return Numerics<accscalar_t>::max(a, b);
          });
    }

    // get sum value
    accscalar_t sum_value = 0;
#pragma unroll(outer_loop)
    for (int i = 0;
         i < outer_loop && ((i * local_stride + lid_offset) < dim_size);
         ++i) {
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        sum_value += Numerics<accscalar_t>::exp(reg_in[i][j] - max_value);
      }
    }
    if (local_size > 1) {
      group_reduce<SIMD, accscalar_t>(
          item_id,
          lid_row,
          sub_group_num,
          sum_value,
          accscalar_t(0),
          local_sum,
          [](accscalar_t a, accscalar_t b) { return a + b; });
    }
    if constexpr (LogSoftMax)
      sum_value = Numerics<accscalar_t>::log(sum_value);
    else if (sum_value != 0)
      sum_value = accscalar_t(1) / sum_value;

      // update result
#pragma unroll(outer_loop)
    for (int i = 0; i < outer_loop; ++i) {
      auto index = i * local_stride + lid_offset;
      if (index >= dim_size)
        break;

#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        if constexpr (LogSoftMax) {
          reg_in[i][j] =
              static_cast<scalar_t>(reg_in[i][j] - max_value - sum_value);
        } else if (sum_value == 0) {
          reg_in[i][j] = nan;
        } else {
          reg_in[i][j] = static_cast<scalar_t>(
              Numerics<accscalar_t>::exp(reg_in[i][j] - max_value) * sum_value);
        }
      }
      *(reinterpret_cast<vec_t*>(out_data + group_offset + index)) = reg_in[i];
    }
  }
  DispatchSoftmaxForwardKernelFunctor(
      scalar_t* in_data_,
      scalar_t* out_data_,
      int dim_size_,
      int outer_size_,
      bool* mask_data_,
      calc_t input_calc_,
      int sub_group_num_,
      int global_size_row_,
      int local_size_row_,
      int range_,
      int local_size_,
      scalar_t neginf_,
      scalar_t nan_,
      dpcpp_local_acc_t<accscalar_t, 2> local_max_,
      dpcpp_local_acc_t<accscalar_t, 2> local_sum_)
      : in_data(in_data_),
        out_data(out_data_),
        dim_size(dim_size_),
        outer_size(outer_size_),
        mask_data(mask_data_),
        input_calc(input_calc_),
        sub_group_num(sub_group_num_),
        global_size_row(global_size_row_),
        local_size_row(local_size_row_),
        range(range_),
        local_size(local_size_),
        neginf(neginf_),
        nan(nan_),
        local_max(local_max_),
        local_sum(local_sum_) {}

 private:
  scalar_t* in_data;
  scalar_t* out_data;
  int dim_size;
  int outer_size;
  bool* mask_data;
  calc_t input_calc;
  int sub_group_num;
  int global_size_row;
  int local_size_row;
  int range;
  int local_size;
  scalar_t neginf;
  scalar_t nan;
  dpcpp_local_acc_t<accscalar_t, 2> local_max;
  dpcpp_local_acc_t<accscalar_t, 2> local_sum;
};

// replace std::nullptr_t to avoid kernel name in std namespace
struct DummyFunctor {};

template <
    int INNER_LOOP,
    int vec_size,
    int SIMD,
    typename scalar_t,
    typename accscalar_t,
    typename IndexType,
    bool LogSoftMax,
    int outer_loop,
    bool is_masked = false,
    typename calc_t = decltype(nullptr)>
void dispatch_softmax_forward_kernel(
    scalar_t* in_data,
    scalar_t* out_data,
    int dim_size,
    int outer_size,
    bool* mask_data = nullptr,
    calc_t input_calc = nullptr) {
  using vec_t = at::native::Memory::aligned_vector_loop<scalar_t, vec_size>;
  auto& dpcpp_queue = dpcppGetCurrentQueue();

  int sub_group_num, global_size_row, local_size_row, range, local_size;
  get_wgroup_size<SIMD, vec_size, outer_loop>(
      dim_size,
      outer_size,
      sub_group_num,
      range,
      global_size_row,
      local_size_row,
      local_size);
  sycl::range<1> local_range{local_size_row * local_size};
  sycl::range<1> global_range{global_size_row * local_size_row * local_size};
  scalar_t neginf = -std::numeric_limits<scalar_t>::infinity();
  scalar_t nan = std::numeric_limits<accscalar_t>::quiet_NaN();
  auto cgf = DPCPP_Q_CGF(cgh) {
    auto local_max = dpcpp_local_acc_t<accscalar_t, 2>(
        sycl::range<2>{local_size_row, sub_group_num}, cgh);
    auto local_sum = dpcpp_local_acc_t<accscalar_t, 2>(
        sycl::range<2>{local_size_row, sub_group_num}, cgh);

    if constexpr (is_masked) {
      DispatchSoftmaxForwardKernelFunctor<
          INNER_LOOP,
          vec_size,
          SIMD,
          scalar_t,
          accscalar_t,
          IndexType,
          LogSoftMax,
          outer_loop,
          is_masked,
          calc_t,
          vec_t>
          kfn(in_data,
              out_data,
              dim_size,
              outer_size,
              mask_data,
              input_calc,
              sub_group_num,
              global_size_row,
              local_size_row,
              range,
              local_size,
              neginf,
              nan,
              local_max,
              local_sum);
      cgh.parallel_for<decltype(kfn)>(
          sycl::nd_range<1>{global_range, local_range}, kfn);
    } else {
      DummyFunctor dummy;
      DispatchSoftmaxForwardKernelFunctor<
          INNER_LOOP,
          vec_size,
          SIMD,
          scalar_t,
          accscalar_t,
          IndexType,
          LogSoftMax,
          outer_loop,
          is_masked,
          DummyFunctor,
          vec_t>
          kfn(in_data,
              out_data,
              dim_size,
              outer_size,
              mask_data,
              dummy,
              sub_group_num,
              global_size_row,
              local_size_row,
              range,
              local_size,
              neginf,
              nan,
              local_max,
              local_sum);
      cgh.parallel_for<decltype(kfn)>(
          sycl::nd_range<1>{global_range, local_range}, kfn);
    }
  };
  // launch kernel
  DPCPP_Q_SUBMIT(dpcpp_queue, cgf);
}

template <
    int vec_size,
    typename scalar_t,
    typename accscalar_t,
    typename IndexType,
    bool LogSoftMax,
    typename vec_t,
    int align_bytes>
struct SoftmaxForwardKernelFunctor {
  void operator()(sycl::nd_item<1> item_id) const {
    IndexType local_id = item_id.get_local_id(0);
    auto group_offset = item_id.get_group(0) * dim_size;
    int start =
        ((uint64_t)(in_data + group_offset)) % align_bytes / sizeof(scalar_t);
    IndexType loops_end = (dim_size + start + vec_size - 1) / vec_size;

    // get max value
    auto max_value = std::numeric_limits<accscalar_t>::lowest();
    for (int i = local_id; i < loops_end; i += local_size) {
      vec_t in_val = *(reinterpret_cast<vec_t*>(
          in_data + group_offset - start + i * vec_size));
#pragma unroll(vec_size)
      for (IndexType j = 0; j < vec_size; ++j) {
        IndexType linear_idx = i * vec_size + j - start;
        if (linear_idx >= 0 && linear_idx < dim_size) {
          scalar_t in_value = in_val[j];
          max_value =
              Numerics<accscalar_t>::max(accscalar_t(in_value), max_value);
        }
      }
    }
    max_value = sycl::reduce_over_group(
        item_id.get_group(), max_value, sycl::maximum<accscalar_t>());

    // get sum value
    auto sum_value = accscalar_t(0);
    for (IndexType i = local_id; i < loops_end; i += local_size) {
      vec_t in_val = *(reinterpret_cast<vec_t*>(
          in_data + group_offset - start + i * vec_size));
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        IndexType linear_idx = i * vec_size + j - start;
        if (linear_idx >= 0 && linear_idx < dim_size)
          sum_value +=
              Numerics<accscalar_t>::exp(accscalar_t(in_val[j]) - max_value);
      }
    }
    sum_value = sycl::reduce_over_group(
        item_id.get_group(), sum_value, sycl::plus<accscalar_t>());
    if (LogSoftMax)
      sum_value = Numerics<accscalar_t>::log(sum_value);
    else
      sum_value = accscalar_t(1) / sum_value;

    // update result
    for (IndexType i = local_id; i < loops_end; i += local_size) {
      auto remaining = dim_size + start - i * vec_size;
      if ((start > 0 && i == 0) || (remaining < vec_size)) {
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j) {
          IndexType linear_idx = i * vec_size + j - start;
          if (linear_idx >= 0 && linear_idx < dim_size) {
            if (LogSoftMax)
              out_data[group_offset + linear_idx] = static_cast<scalar_t>(
                  in_data[group_offset + linear_idx] - max_value - sum_value);
            else
              out_data[group_offset + linear_idx] = static_cast<scalar_t>(
                  Numerics<accscalar_t>::exp(
                      in_data[group_offset + linear_idx] - max_value) *
                  sum_value);
          }
        }
      } else {
        vec_t in_val = *(reinterpret_cast<vec_t*>(
            in_data + group_offset - start + i * vec_size));
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j) {
          if (LogSoftMax)
            in_val[j] =
                static_cast<scalar_t>(in_val[j] - max_value - sum_value);
          else
            in_val[j] = static_cast<scalar_t>(
                Numerics<accscalar_t>::exp(in_val[j] - max_value) * sum_value);
        }
        *(reinterpret_cast<vec_t*>(
            out_data + group_offset - start + i * vec_size)) = in_val;
      }
    }
  }
  SoftmaxForwardKernelFunctor(
      scalar_t* in_data_,
      scalar_t* out_data_,
      int dim_size_,
      int outer_size_,
      int local_size_)
      : in_data(in_data_),
        out_data(out_data_),
        dim_size(dim_size_),
        outer_size(outer_size_),
        local_size(local_size_) {}

 private:
  scalar_t* in_data;
  scalar_t* out_data;
  int dim_size;
  int outer_size;
  int local_size;
};

template <
    int vec_size,
    typename scalar_t,
    typename accscalar_t,
    typename IndexType,
    bool LogSoftMax>
void softmax_forward_kernel(
    scalar_t* in_data,
    scalar_t* out_data,
    int dim_size,
    int outer_size) {
  using vec_t = at::native::Memory::aligned_vector_loop<scalar_t, vec_size>;
  constexpr int align_bytes = alignof(vec_t);
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  int local_size = std::min(
      (dim_size + vec_size - 1) / vec_size, int(dpcppMaxWorkGroupSize(dev_id)));

  sycl::range<1> local_range{local_size};
  sycl::range<1> global_range{local_size * outer_size};
  auto cgf = DPCPP_Q_CGF(cgh) {
    SoftmaxForwardKernelFunctor<
        vec_size,
        scalar_t,
        accscalar_t,
        IndexType,
        LogSoftMax,
        vec_t,
        align_bytes>
        kfn(in_data, out_data, dim_size, outer_size, local_size);
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<1>(global_range, local_range), kfn);
  };

  // launch kernel
  DPCPP_Q_SUBMIT(dpcpp_queue, cgf);
}

template <
    int INNER_LOOP,
    int vec_size,
    int SIMD,
    typename scalar_t,
    typename accscalar_t,
    typename IndexType,
    bool LogSoftMax,
    int outer_loop,
    typename inp_offset_calc_t,
    typename vec_t>
struct DispatchSoftmaxForwardAddKernelFunctor {
  [[intel::reqd_sub_group_size(SIMD)]] void operator()(
      sycl::nd_item<1> item_id) const {
    if (local_size == 1 && item_id.get_global_id(0) >= outer_size)
      return;

    uint32_t lid_row = 0;
    uint32_t lid_col = item_id.get_local_id(0);
    uint32_t group_offset = item_id.get_group(0) * dim_size;
    if (local_size_row != 1) {
      lid_row = item_id.get_local_id(0) / local_size;
      lid_col = item_id.get_local_id(0) % local_size;
      group_offset =
          (item_id.get_group(0) * local_size_row + lid_row) * dim_size;
    }
    vec_t reg_in[outer_loop];
    vec_t reg_tmp;
    auto lid_offset = lid_col * vec_size;
    auto local_stride = local_size * vec_size;
    // load data and get max value
    accscalar_t max_value = std::numeric_limits<accscalar_t>::lowest();
#pragma unroll(outer_loop)
    for (int i = 0; i < outer_loop; ++i) {
      auto index = i * local_stride + lid_offset;
      if (index >= dim_size)
        break;

      auto group_batch_offset = group_offset + index;
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        auto linear_offset = group_batch_offset + j;
        scalar_t input_value = in_data[input_calc.get(linear_offset)[0]];
        scalar_t other_value = other_data[input_calc.get(linear_offset)[1]];
        reg_in[i][j] = input_value + alpha * other_value;
      }

#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        max_value =
            Numerics<accscalar_t>::max(max_value, accscalar_t(reg_in[i][j]));
      }
    }
    if (local_size > 1) {
      group_reduce<SIMD, accscalar_t>(
          item_id,
          lid_row,
          sub_group_num,
          max_value,
          std::numeric_limits<accscalar_t>::lowest(),
          local_max,
          [](accscalar_t a, accscalar_t b) {
            return Numerics<accscalar_t>::max(a, b);
          });
    }

    // get sum value
    accscalar_t sum_value = 0;
#pragma unroll(outer_loop)
    for (int i = 0;
         i < outer_loop && ((i * local_stride + lid_offset) < dim_size);
         ++i) {
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        sum_value += Numerics<accscalar_t>::exp(reg_in[i][j] - max_value);
      }
    }
    if (local_size > 1) {
      group_reduce<SIMD, accscalar_t>(
          item_id,
          lid_row,
          sub_group_num,
          sum_value,
          accscalar_t(0),
          local_sum,
          [](accscalar_t a, accscalar_t b) { return a + b; });
    }
    if constexpr (LogSoftMax)
      sum_value = Numerics<accscalar_t>::log(sum_value);
    else
      sum_value = accscalar_t(1) / sum_value;

      // update result
#pragma unroll(outer_loop)
    for (int i = 0; i < outer_loop; ++i) {
      auto index = i * local_stride + lid_offset;
      if (index >= dim_size)
        break;

#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        if constexpr (LogSoftMax) {
          reg_in[i][j] =
              static_cast<scalar_t>(reg_in[i][j] - max_value - sum_value);
        } else {
          reg_in[i][j] = static_cast<scalar_t>(
              Numerics<accscalar_t>::exp(reg_in[i][j] - max_value) * sum_value);
        }
      }
      *(reinterpret_cast<vec_t*>(out_data + group_offset + index)) = reg_in[i];
    }
  }
  DispatchSoftmaxForwardAddKernelFunctor(
      scalar_t* in_data_,
      scalar_t* other_data_,
      scalar_t* out_data_,
      int dim_size_,
      scalar_t alpha_,
      int outer_size_,
      int other_outer_size_,
      inp_offset_calc_t input_calc_,
      int sub_group_num_,
      int global_size_row_,
      int local_size_row_,
      int range_,
      int local_size_,
      int other_offset_,
      dpcpp_local_acc_t<accscalar_t, 2> local_max_,
      dpcpp_local_acc_t<accscalar_t, 2> local_sum_)
      : in_data(in_data_),
        other_data(other_data_),
        out_data(out_data_),
        dim_size(dim_size_),
        alpha(alpha_),
        outer_size(outer_size_),
        other_outer_size(other_outer_size_),
        input_calc(input_calc_),
        sub_group_num(sub_group_num_),
        global_size_row(global_size_row_),
        local_size_row(local_size_row_),
        range(range_),
        local_size(local_size_),
        other_offset(other_offset_),
        local_max(local_max_),
        local_sum(local_sum_) {}

 private:
  scalar_t* in_data;
  scalar_t* other_data;
  scalar_t* out_data;
  int dim_size;
  scalar_t alpha;
  int outer_size;
  int other_outer_size;
  inp_offset_calc_t input_calc;
  int sub_group_num;
  int global_size_row;
  int local_size_row;
  int range;
  int local_size;
  int other_offset;
  dpcpp_local_acc_t<accscalar_t, 2> local_max;
  dpcpp_local_acc_t<accscalar_t, 2> local_sum;
};

template <
    int INNER_LOOP,
    int vec_size,
    int SIMD,
    typename scalar_t,
    typename accscalar_t,
    typename IndexType,
    bool LogSoftMax,
    int outer_loop,
    typename inp_offset_calc_t>
void dispatch_softmax_forward_add_kernel(
    scalar_t* in_data,
    scalar_t* other_data,
    scalar_t* out_data,
    int dim_size,
    scalar_t alpha,
    int outer_size,
    int other_outer_size,
    inp_offset_calc_t input_calc) {
  using vec_t = at::native::Memory::aligned_vector_loop<scalar_t, vec_size>;
  auto& dpcpp_queue = dpcppGetCurrentQueue();

  int sub_group_num, global_size_row, local_size_row, range, local_size;
  get_wgroup_size<SIMD, vec_size, outer_loop>(
      dim_size,
      outer_size,
      sub_group_num,
      range,
      global_size_row,
      local_size_row,
      local_size);
  sycl::range<1> local_range{local_size_row * local_size};
  sycl::range<1> global_range{global_size_row * local_size_row * local_size};
  auto other_offset = other_outer_size * dim_size;
  auto cgf = DPCPP_Q_CGF(cgh) {
    auto local_max = dpcpp_local_acc_t<accscalar_t, 2>(
        sycl::range<2>{local_size_row, sub_group_num}, cgh);
    auto local_sum = dpcpp_local_acc_t<accscalar_t, 2>(
        sycl::range<2>{local_size_row, sub_group_num}, cgh);

    DispatchSoftmaxForwardAddKernelFunctor<
        INNER_LOOP,
        vec_size,
        SIMD,
        scalar_t,
        accscalar_t,
        IndexType,
        LogSoftMax,
        outer_loop,
        inp_offset_calc_t,
        vec_t>
        kfn(in_data,
            other_data,
            out_data,
            dim_size,
            alpha,
            outer_size,
            other_outer_size,
            input_calc,
            sub_group_num,
            global_size_row,
            local_size_row,
            range,
            local_size,
            other_offset,
            local_max,
            local_sum);
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<1>{global_range, local_range}, kfn);
  };
  // launch kernel
  DPCPP_Q_SUBMIT(dpcpp_queue, cgf);
}

template <
    int vec_size,
    typename scalar_t,
    typename accscalar_t,
    typename IndexType,
    bool LogSoftMax,
    typename vec_t>
struct SpatialSoftmaxForwardKernelFunctor {
  void operator()(sycl::nd_item<3> item_id) const {
    IndexType global_col = item_id.get_global_id(2);
    IndexType local_row_id = item_id.get_local_id(1);
    IndexType local_col_id = item_id.get_local_id(2);

    auto group_offset = item_id.get_global_id(0) * dim_size * inner_size;
    auto out_ptr = out_data + group_offset;

    // get max value
    accscalar_t max_value[vec_size];
    auto offset = local_row_id * inner_size + global_col * vec_size;
    vec_t value = *(reinterpret_cast<vec_t*>(in_data + group_offset + offset));
#pragma unroll(vec_size)
    for (int j = 0; j < vec_size; ++j) {
      max_value[j] = accscalar_t(value[j]);
    }
    for (int i = local_row_id + block_row; i < dim_size; i += block_row) {
      offset = i * inner_size + global_col * vec_size;
      value = *(reinterpret_cast<vec_t*>(in_data + group_offset + offset));
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        max_value[j] =
            Numerics<accscalar_t>::max(max_value[j], accscalar_t(value[j]));
      }
    }
    if (block_row > 1) {
      group_reduce_spatial<vec_size, accscalar_t>(
          item_id,
          max_value,
          local_data,
          block_row,
          [](accscalar_t a, accscalar_t b) {
            return Numerics<accscalar_t>::max(a, b);
          });
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        max_value[j] = local_data[0][local_col_id][j];
      }
      item_id.barrier();
    }

    // get sum value
    accscalar_t sum_value[vec_size];
    offset = local_row_id * inner_size + global_col * vec_size;
    value = *(reinterpret_cast<vec_t*>(in_data + group_offset + offset));
#pragma unroll(vec_size)
    for (int j = 0; j < vec_size; ++j) {
      sum_value[j] = Numerics<accscalar_t>::exp(value[j] - max_value[j]);
    }
    for (int i = local_row_id + block_row; i < dim_size; i += block_row) {
      offset = i * inner_size + global_col * vec_size;
      value = *(reinterpret_cast<vec_t*>(in_data + group_offset + offset));
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        sum_value[j] += Numerics<accscalar_t>::exp(value[j] - max_value[j]);
      }
    }
    if (block_row > 1) {
      group_reduce_spatial<vec_size, accscalar_t>(
          item_id,
          sum_value,
          local_data,
          block_row,
          [](accscalar_t a, accscalar_t b) { return a + b; });
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        if (LogSoftMax)
          sum_value[j] =
              Numerics<accscalar_t>::log(local_data[0][local_col_id][j]);
        else
          sum_value[j] = accscalar_t(1) / local_data[0][local_col_id][j];
      }
    } else {
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        if (LogSoftMax)
          sum_value[j] = Numerics<accscalar_t>::log(sum_value[j]);
        else
          sum_value[j] = accscalar_t(1) / sum_value[j];
      }
    }

    // update result
    if (global_col * vec_size < inner_size) {
      for (int i = local_row_id; i < dim_size; i += block_row) {
        auto offset = i * inner_size + global_col * vec_size;
        vec_t in_val =
            *(reinterpret_cast<vec_t*>(in_data + group_offset + offset));
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j) {
          if (LogSoftMax)
            in_val[j] =
                static_cast<scalar_t>(in_val[j] - max_value[j] - sum_value[j]);
          else
            in_val[j] = static_cast<scalar_t>(
                Numerics<accscalar_t>::exp(in_val[j] - max_value[j]) *
                sum_value[j]);
        }
        *(reinterpret_cast<vec_t*>(out_data + group_offset + offset)) = in_val;
      }
    }
  }
  SpatialSoftmaxForwardKernelFunctor(
      scalar_t* in_data_,
      scalar_t* out_data_,
      int dim_size_,
      int inner_size_,
      int outer_size_,
      int local_size_,
      int block_row_,
      int group_num_,
      dpcpp_local_acc_t<accscalar_t, 3> local_data_)
      : in_data(in_data_),
        out_data(out_data_),
        dim_size(dim_size_),
        inner_size(inner_size_),
        outer_size(outer_size_),
        local_size(local_size_),
        block_row(block_row_),
        group_num(group_num_),
        local_data(local_data_) {}

 private:
  scalar_t* in_data;
  scalar_t* out_data;
  int dim_size;
  int inner_size;
  int outer_size;
  int local_size;
  int block_row;
  int group_num;
  dpcpp_local_acc_t<accscalar_t, 3> local_data;
};

template <
    int vec_size,
    typename scalar_t,
    typename accscalar_t,
    typename IndexType,
    bool LogSoftMax>
void spatial_softmax_forward(
    scalar_t* in_data,
    scalar_t* out_data,
    int dim_size,
    int inner_size,
    int outer_size) {
  using vec_t = at::native::Memory::aligned_vector_loop<scalar_t, vec_size>;
  auto& dpcpp_queue = dpcppGetCurrentQueue();

  int local_size, block_row;
  get_wgroup_size_spatial<vec_size>(
      outer_size, dim_size, inner_size, local_size, block_row);
  int group_num =
      (inner_size + local_size * vec_size - 1) / (local_size * vec_size);
  sycl::range<3> global_range{outer_size, block_row, group_num * local_size};
  sycl::range<3> local_range{1, block_row, local_size};
  auto cgf = DPCPP_Q_CGF(cgh) {
    auto local_data = dpcpp_local_acc_t<accscalar_t, 3>(
        sycl::range<3>{block_row, local_size, vec_size}, cgh);
    SpatialSoftmaxForwardKernelFunctor<
        vec_size,
        scalar_t,
        accscalar_t,
        IndexType,
        LogSoftMax,
        vec_t>
        kfn(in_data,
            out_data,
            dim_size,
            inner_size,
            outer_size,
            local_size,
            block_row,
            group_num,
            local_data);
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<3>(global_range, local_range), kfn);
  };

  // launch kernel
  DPCPP_Q_SUBMIT(dpcpp_queue, cgf);
}

template <
    int INNER_LOOP,
    int vec_size,
    int SIMD,
    typename scalar_t,
    typename accscalar_t,
    typename IndexType,
    bool LogSoftMax,
    bool is_masked,
    typename calc_t,
    typename vec_t,
    int NUM>
struct DispatchSoftmaxBackwardKernelFunctor {
  [[intel::reqd_sub_group_size(SIMD)]] void operator()(
      sycl::nd_item<1> item_id) const {
    if (local_size == 1 && item_id.get_global_id(0) >= outer_size)
      return;

    uint32_t lid_row = item_id.get_local_id(0) / local_size;
    uint32_t lid_col = item_id.get_local_id(0) % local_size;
    uint32_t group_offset =
        (item_id.get_group(0) * local_size_row + lid_row) * dim_size;

    // load data and get max value
    accscalar_t sum_value = accscalar_t(0);
    vec_t reg_out[NUM];
    vec_t reg_gradout[NUM];
#pragma unroll(NUM)
    for (int i = 0; i < NUM; ++i) {
      auto index = (lid_col + i * local_size) * vec_size;
      if (index >= dim_size)
        break;

      reg_out[i] = *(reinterpret_cast<vec_t*>(output + group_offset + index));
      reg_gradout[i] =
          *(reinterpret_cast<vec_t*>(gradOutput + group_offset + index));
      if constexpr (is_masked) {
        auto vec_offset = group_offset + index;
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j) {
          auto linear_idx = vec_offset + j;
          auto mask_offset = input_calc.get(linear_idx)[1];
          if (mask_data[mask_offset]) {
            reg_out[i][j] = scalar_t(0);
          }
        }
      }

#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        if (LogSoftMax) {
          sum_value += reg_gradout[i][j];
        } else {
          sum_value += reg_out[i][j] * reg_gradout[i][j];
        }
      }
    }
    if (local_size > 1) {
      group_reduce<SIMD, accscalar_t>(
          item_id,
          lid_row,
          sub_group_num,
          sum_value,
          accscalar_t(0),
          local_sum,
          [](accscalar_t a, accscalar_t b) { return a + b; });
    }
    // update result
#pragma unroll(NUM)
    for (int i = 0; i < NUM; ++i) {
      auto index = (lid_col + i * local_size) * vec_size;
      if (index >= dim_size)
        break;

#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        if (LogSoftMax) {
          reg_out[i][j] = static_cast<scalar_t>(
              reg_gradout[i][j] -
              Numerics<accscalar_t>::exp(reg_out[i][j]) * sum_value);
        } else {
          reg_out[i][j] = static_cast<scalar_t>(
              reg_out[i][j] * (reg_gradout[i][j] - sum_value));
        }
      }
      *(reinterpret_cast<vec_t*>(gradInput + group_offset + index)) =
          reg_out[i];
    }
  }
  DispatchSoftmaxBackwardKernelFunctor(
      scalar_t* gradInput_,
      scalar_t* output_,
      scalar_t* gradOutput_,
      int dim_size_,
      int outer_size_,
      bool* mask_data_,
      calc_t input_calc_,
      int sub_group_num_,
      int global_size_row_,
      int local_size_row_,
      int range_,
      int local_size_,
      dpcpp_local_acc_t<accscalar_t, 2> local_sum_)
      : gradInput(gradInput_),
        output(output_),
        gradOutput(gradOutput_),
        dim_size(dim_size_),
        outer_size(outer_size_),
        mask_data(mask_data_),
        input_calc(input_calc_),
        sub_group_num(sub_group_num_),
        global_size_row(global_size_row_),
        local_size_row(local_size_row_),
        range(range_),
        local_size(local_size_),
        local_sum(local_sum_) {}

 private:
  scalar_t* gradInput;
  scalar_t* output;
  scalar_t* gradOutput;
  int dim_size;
  int outer_size;
  bool* mask_data;
  calc_t input_calc;
  int sub_group_num;
  int global_size_row;
  int local_size_row;
  int range;
  int local_size;
  dpcpp_local_acc_t<accscalar_t, 2> local_sum;
};

template <
    int INNER_LOOP,
    int vec_size,
    int SIMD,
    typename scalar_t,
    typename accscalar_t,
    typename IndexType,
    bool LogSoftMax,
    bool is_masked = false,
    typename calc_t = decltype(nullptr)>
void dispatch_softmax_backward_kernel(
    scalar_t* gradInput,
    scalar_t* output,
    scalar_t* gradOutput,
    int dim_size,
    int outer_size,
    bool* mask_data = nullptr,
    calc_t input_calc = nullptr) {
  using vec_t = at::native::Memory::aligned_vector_loop<scalar_t, vec_size>;
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  constexpr int NUM = INNER_LOOP / vec_size * (SIMD32 / SIMD);
  int sub_group_num, global_size_row, local_size_row, range, local_size;
  get_wgroup_size<SIMD, vec_size, NUM>(
      dim_size,
      outer_size,
      sub_group_num,
      range,
      global_size_row,
      local_size_row,
      local_size);
  sycl::range<1> local_range{local_size_row * local_size};
  sycl::range<1> global_range{global_size_row * local_size_row * local_size};

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto local_sum = dpcpp_local_acc_t<accscalar_t, 2>(
        sycl::range<2>{local_size_row, sub_group_num}, cgh);
    if constexpr (is_masked) {
      DispatchSoftmaxBackwardKernelFunctor<
          INNER_LOOP,
          vec_size,
          SIMD,
          scalar_t,
          accscalar_t,
          IndexType,
          LogSoftMax,
          is_masked,
          calc_t,
          vec_t,
          NUM>
          kfn(gradInput,
              output,
              gradOutput,
              dim_size,
              outer_size,
              mask_data,
              input_calc,
              sub_group_num,
              global_size_row,
              local_size_row,
              range,
              local_size,
              local_sum);
      cgh.parallel_for<decltype(kfn)>(
          sycl::nd_range<1>(global_range, local_range), kfn);
    } else {
      DummyFunctor dummy;
      DispatchSoftmaxBackwardKernelFunctor<
          INNER_LOOP,
          vec_size,
          SIMD,
          scalar_t,
          accscalar_t,
          IndexType,
          LogSoftMax,
          is_masked,
          DummyFunctor,
          vec_t,
          NUM>
          kfn(gradInput,
              output,
              gradOutput,
              dim_size,
              outer_size,
              mask_data,
              dummy,
              sub_group_num,
              global_size_row,
              local_size_row,
              range,
              local_size,
              local_sum);
      cgh.parallel_for<decltype(kfn)>(
          sycl::nd_range<1>(global_range, local_range), kfn);
    }
  };
  // launch kernel
  DPCPP_Q_SUBMIT(dpcpp_queue, cgf);
}

template <
    int vec_size,
    typename scalar_t,
    typename accscalar_t,
    bool LogSoftMax,
    typename vec_t,
    int align_bytes>
struct SoftmaxBackwardKernelFunctor {
  void operator()(sycl::nd_item<1> item_id) const {
    int local_id = item_id.get_local_id(0);
    auto group_offset = item_id.get_group(0) * dim_size;
    int start =
        ((uint64_t)(output + group_offset)) % align_bytes / sizeof(scalar_t);
    int loops_end = (dim_size + start + vec_size - 1) / vec_size;

    vec_t* vec_gradin_data_ptr =
        reinterpret_cast<vec_t*>(gradInput + group_offset - start);
    const vec_t* vec_out_data_ptr =
        reinterpret_cast<const vec_t*>(output + group_offset - start);
    const vec_t* vec_gradout_data_ptr =
        reinterpret_cast<const vec_t*>(gradOutput + group_offset - start);

    // get sum value
    auto sum_value = accscalar_t(0);
    for (int i = local_id; i < loops_end; i += local_size) {
      auto gradout_val = vec_gradout_data_ptr[i];
      if (LogSoftMax) {
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j) {
          int64_t linear_idx = i * vec_size + j - start;
          if (linear_idx >= 0 && linear_idx < dim_size) {
            sum_value += gradout_val[j];
          }
        }
      } else {
        vec_t out_val = vec_out_data_ptr[i];
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j) {
          int64_t linear_idx = i * vec_size + j - start;
          if (linear_idx >= 0 && linear_idx < dim_size) {
            sum_value += out_val[j] * gradout_val[j];
          }
        }
      }
    }
    sum_value = sycl::reduce_over_group(
        item_id.get_group(), sum_value, sycl::plus<accscalar_t>());

    // update result
    for (int i = local_id; i < loops_end; i += local_size) {
      // handle the head and tail
      auto remaining = dim_size + start - i * vec_size;
      if ((start > 0 && i == 0) || (remaining < vec_size)) {
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j) {
          auto linear_idx = i * vec_size + j - start;
          if (linear_idx >= 0 && linear_idx < dim_size) {
            auto offset = group_offset + linear_idx;
            if (LogSoftMax) {
              gradInput[offset] = gradOutput[offset] -
                  Numerics<accscalar_t>::exp(output[offset]) * sum_value;
            } else {
              gradInput[offset] =
                  output[offset] * (gradOutput[offset] - sum_value);
            }
          }
        }
      } else {
        vec_t grad_val = vec_gradout_data_ptr[i];
        vec_t out_val = vec_out_data_ptr[i];
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j) {
          if (LogSoftMax) {
            out_val[j] = grad_val[j] -
                Numerics<accscalar_t>::exp(out_val[j]) * sum_value;
          } else {
            out_val[j] = out_val[j] * (grad_val[j] - sum_value);
          }
        }
        vec_gradin_data_ptr[i] = out_val;
      }
    }
  }
  SoftmaxBackwardKernelFunctor(
      scalar_t* gradInput_,
      const scalar_t* output_,
      const scalar_t* gradOutput_,
      int dim_size_,
      int outer_size_,
      int local_size_)
      : gradInput(gradInput_),
        output(output_),
        gradOutput(gradOutput_),
        dim_size(dim_size_),
        outer_size(outer_size_),
        local_size(local_size_) {}

 private:
  scalar_t* gradInput;
  const scalar_t* output;
  const scalar_t* gradOutput;
  int dim_size;
  int outer_size;
  int local_size;
};

template <
    int vec_size,
    typename scalar_t,
    typename accscalar_t,
    bool LogSoftMax>
void softmax_backward_kernel(
    scalar_t* gradInput,
    const scalar_t* output,
    const scalar_t* gradOutput,
    int dim_size,
    int outer_size) {
  using vec_t = at::native::Memory::aligned_vector_loop<scalar_t, vec_size>;
  constexpr int align_bytes = alignof(vec_t);
  auto& dpcpp_queue = dpcppGetCurrentQueue();

  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  int local_size = std::min(
      (dim_size + vec_size - 1) / vec_size, int(dpcppMaxWorkGroupSize(dev_id)));
  sycl::range<1> local_range{local_size};
  sycl::range<1> global_range{local_size * outer_size};

  auto cgf = DPCPP_Q_CGF(cgh) {
    SoftmaxBackwardKernelFunctor<
        vec_size,
        scalar_t,
        accscalar_t,
        LogSoftMax,
        vec_t,
        align_bytes>
        kfn(gradInput, output, gradOutput, dim_size, outer_size, local_size);
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<1>(global_range, local_range), kfn);
  };

  // launch kernel
  DPCPP_Q_SUBMIT(dpcpp_queue, cgf);
}

template <
    int vec_size,
    typename scalar_t,
    typename accscalar_t,
    bool LogSoftMax,
    typename vec_t>
struct SpatialSoftmaxBackwardKernelFunctor {
  void operator()(sycl::nd_item<3> item_id) const {
    auto global_col = item_id.get_global_id(2);
    auto local_row_id = item_id.get_local_id(1);
    auto local_col_id = item_id.get_local_id(2);

    auto group_offset = item_id.get_global_id(0) * dim_size * inner_size;
    auto gradin_ptr = gradInput + group_offset;
    auto out_ptr = output + group_offset;
    auto gradout_ptr = gradOutput + group_offset;

    // get sum value
    accscalar_t sum_value[vec_size];
#pragma unroll(vec_size)
    for (int j = 0; j < vec_size; ++j)
      sum_value[j] = accscalar_t(0);

    for (int i = local_row_id; i < dim_size; i += block_row) {
      auto offset = i * inner_size + global_col * vec_size;
      vec_t gradout_val =
          *(reinterpret_cast<const vec_t*>(gradout_ptr + offset));
      if (LogSoftMax) {
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j)
          sum_value[j] += gradout_val[j];
      } else {
        vec_t out_val = *(reinterpret_cast<const vec_t*>(out_ptr + offset));
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j)
          sum_value[j] += accscalar_t(gradout_val[j]) * out_val[j];
      }
    }
    if (block_row > 1) {
      group_reduce_spatial<vec_size, accscalar_t>(
          item_id,
          sum_value,
          local_data,
          block_row,
          [](accscalar_t a, accscalar_t b) { return a + b; });
#pragma unroll(vec_size)
      for (int j = 0; j < vec_size; ++j) {
        sum_value[j] = local_data[0][local_col_id][j];
      }
    }

    // update result
    if (global_col * vec_size < inner_size) {
      for (int i = local_row_id; i < dim_size; i += block_row) {
        auto offset = i * inner_size + global_col * vec_size;
        vec_t out_val = *(reinterpret_cast<const vec_t*>(out_ptr + offset));
        vec_t gradout_val =
            *(reinterpret_cast<const vec_t*>(gradout_ptr + offset));
#pragma unroll(vec_size)
        for (int j = 0; j < vec_size; ++j) {
          if (LogSoftMax) {
            out_val[j] = static_cast<scalar_t>(
                gradout_val[j] -
                Numerics<accscalar_t>::exp(out_val[j]) * sum_value[j]);
          } else {
            out_val[j] = static_cast<scalar_t>(
                out_val[j] * (gradout_val[j] - sum_value[j]));
          }
        }
        *(reinterpret_cast<vec_t*>(gradin_ptr + offset)) = out_val;
      }
    }
  }
  SpatialSoftmaxBackwardKernelFunctor(
      scalar_t* gradInput_,
      const scalar_t* output_,
      const scalar_t* gradOutput_,
      int dim_size_,
      int inner_size_,
      int outer_size_,
      int local_size_,
      int block_row_,
      dpcpp_local_acc_t<accscalar_t, 3> local_data_)
      : gradInput(gradInput_),
        output(output_),
        gradOutput(gradOutput_),
        dim_size(dim_size_),
        inner_size(inner_size_),
        outer_size(outer_size_),
        local_size(local_size_),
        block_row(block_row_),
        local_data(local_data_) {}

 private:
  scalar_t* gradInput;
  const scalar_t* output;
  const scalar_t* gradOutput;
  int dim_size;
  int inner_size;
  int outer_size;
  int local_size;
  int block_row;
  dpcpp_local_acc_t<accscalar_t, 3> local_data;
};

template <
    int vec_size,
    typename scalar_t,
    typename accscalar_t,
    bool LogSoftMax>
void spatial_softmax_backward_kernel(
    scalar_t* gradInput,
    const scalar_t* output,
    const scalar_t* gradOutput,
    int dim_size,
    int inner_size,
    int outer_size) {
  using vec_t = at::native::Memory::aligned_vector_loop<scalar_t, vec_size>;
  auto& dpcpp_queue = dpcppGetCurrentQueue();

  int local_size, block_row;
  get_wgroup_size_spatial<vec_size>(
      outer_size, dim_size, inner_size, local_size, block_row);
  int group_num =
      (inner_size + local_size * vec_size - 1) / (local_size * vec_size);
  sycl::range<3> global_range{outer_size, block_row, group_num * local_size};
  sycl::range<3> local_range{1, block_row, local_size};

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto local_data = dpcpp_local_acc_t<accscalar_t, 3>(
        sycl::range<3>{block_row, local_size, vec_size}, cgh);
    SpatialSoftmaxBackwardKernelFunctor<
        vec_size,
        scalar_t,
        accscalar_t,
        LogSoftMax,
        vec_t>
        kfn(gradInput,
            output,
            gradOutput,
            dim_size,
            inner_size,
            outer_size,
            local_size,
            block_row,
            local_data);
    cgh.parallel_for<decltype(kfn)>(
        sycl::nd_range<3>(global_range, local_range), kfn);
  };

  // launch kernel
  DPCPP_Q_SUBMIT(dpcpp_queue, cgf);
}

template <typename scalar_t, typename accscalar_t, bool LogSoftMax>
Tensor& MaskedSoftMaxForward(
    Tensor& output,
    Tensor& input,
    int dim,
    const Tensor mask) {
  auto inner_size = input.stride(dim);
  auto dim_size = input.size(dim);
  auto outer_size = input.numel() / (inner_size * dim_size);

  constexpr int float4_size = sizeof(float) * 4;
  constexpr int max_vec_size = float4_size / sizeof(scalar_t);
  constexpr int INNER_LOOP = max_vec_size * 2;

  // decide vec_size: max_vec_size or 1
  using vec_t = at::native::Memory::aligned_vector_loop<scalar_t, max_vec_size>;
  constexpr int align_bytes = alignof(vec_t);
  int input_start =
      ((uint64_t)input.data_ptr()) % align_bytes / sizeof(scalar_t);
  int output_start =
      ((uint64_t)output.data_ptr()) % align_bytes / sizeof(scalar_t);

  // decide indexing range: uint32_t (4GB) or uint64_t (>4GB)
  bool can_use_32bit_index =
      canUse32BitIndexMath(input) && canUse32BitIndexMath(output);

  // decide SIMD: SIMD32 or SIMD16
  auto* dev_prop = dpcppGetDeviceProperties(dpcppGetDeviceIdOfCurrentQueue());
  auto sub_group_size = dev_prop->subgroup_sizes;
  int SIMD = sub_group_size[1];
  if (SIMD == SIMD32) {
    if (dim_size < SIMD16 * INNER_LOOP)
      SIMD = SIMD16;
  }

#define DISPATCH_MASK_SOFTMAX_FORWARD_IMPL(vec_size, SIMD, outer_loop) \
  {                                                                    \
    dispatch_softmax_forward_kernel<                                   \
        INNER_LOOP,                                                    \
        vec_size,                                                      \
        SIMD,                                                          \
        scalar_t,                                                      \
        accscalar_t,                                                   \
        uint32_t,                                                      \
        LogSoftMax,                                                    \
        outer_loop,                                                    \
        true,                                                          \
        decltype(input_calc)>(                                         \
        input.data_ptr<scalar_t>(),                                    \
        output.data_ptr<scalar_t>(),                                   \
        dim_size,                                                      \
        outer_size,                                                    \
        mask.data_ptr<bool>(),                                         \
        input_calc);                                                   \
  }

  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  int max_group_size = dpcppMaxWorkGroupSize(dev_id);
  if (inner_size == 1 && can_use_32bit_index &&
      max_group_size * INNER_LOOP >= dim_size) {
    // if the element number is smaller than max_work_group_size * INNER_LOOP,
    // the fast path (dispatch_softmax_forward) will be selected.
    // otherwise, the general path (softmax_forward_kernel) will be selected.
    // it assumes vec_size * outer_loop * work_group_size >= dim_size
    auto iter = TensorIterator::binary_op(output, input, mask);
    auto input_calc = make_input_offset_calculator<2>(iter);

    if (SIMD == SIMD32) {
      // Ensure input/output tensor are aligned with max_vec_size
      if (input_start == 0 && output_start == 0 &&
          dim_size % max_vec_size == 0) {
        constexpr int outer_loop = INNER_LOOP / max_vec_size;
        DISPATCH_MASK_SOFTMAX_FORWARD_IMPL(
            /*vec_size*/ max_vec_size, /*SIMD*/ SIMD32, outer_loop);
      } else {
        constexpr int outer_loop = INNER_LOOP;
        DISPATCH_MASK_SOFTMAX_FORWARD_IMPL(
            /*vec_size*/ 1, /*SIMD*/ SIMD32, outer_loop);
      }
    } else {
      if (input_start == 0 && output_start == 0 &&
          dim_size % max_vec_size == 0) {
        if (max_vec_size >= 4 && dim_size <= 4 * SIMD) {
          // if vec_size >= 4 and dim_size <= 4 * SIMD, take smaller vec_size
          // and 1 outer_loop
          constexpr int outer_loop = 1;
          DISPATCH_MASK_SOFTMAX_FORWARD_IMPL(
              /*vec_size*/ 4, /*SIMD*/ SIMD16, outer_loop);
        } else if (dim_size <= max_vec_size * SIMD) {
          // if dim_size <= max_vec_size * SIMD , take 1 outer_loop
          constexpr int outer_loop = 1;
          DISPATCH_MASK_SOFTMAX_FORWARD_IMPL(
              /*vec_size*/ max_vec_size, /*SIMD*/ SIMD16, outer_loop);
        } else {
          // SIMD16 will use less register numbers than SIMD32
          // if the SIMD = SIMD16, then outer_loop will be enlarged 2x
          constexpr int outer_loop = INNER_LOOP / max_vec_size * 2;
          DISPATCH_MASK_SOFTMAX_FORWARD_IMPL(
              /*vec_size*/ max_vec_size, /*SIMD*/ SIMD16, outer_loop);
        }
      } else {
        constexpr int outer_loop = INNER_LOOP * 2;
        DISPATCH_MASK_SOFTMAX_FORWARD_IMPL(
            /*vec_size*/ 1, /*SIMD*/ SIMD16, outer_loop);
      }
    }
  } else {
    auto mask_expand = mask.expand(input.sizes());
    output = at::softmax_out(
        output,
        input.masked_fill(
            mask_expand, -std::numeric_limits<scalar_t>::infinity()),
        dim);
  }
  return output;
#undef DISPATCH_MASK_SOFTMAX_FORWARD_IMPL
}

template <typename scalar_t, typename accscalar_t, bool LogSoftMax>
void SpatialSoftMaxForward(Tensor& output, Tensor& input, int dim) {
  auto inner_size = input.stride(dim);
  auto dim_size = input.size(dim);
  auto outer_size = input.numel() / (inner_size * dim_size);

  constexpr int float4_size = sizeof(float) * 4;
  constexpr int max_vec_size = float4_size / sizeof(scalar_t);
  constexpr int INNER_LOOP = max_vec_size * 2;

  // decide vec_size: max_vec_size or 1
  using vec_t = at::native::Memory::aligned_vector_loop<scalar_t, max_vec_size>;
  constexpr int align_bytes = alignof(vec_t);
  int input_start =
      ((uint64_t)input.data_ptr()) % align_bytes / sizeof(scalar_t);
  int output_start =
      ((uint64_t)output.data_ptr()) % align_bytes / sizeof(scalar_t);

  // decide indexing range: uint32_t (4GB) or uint64_t (>4GB)
  bool can_use_32bit_index =
      canUse32BitIndexMath(input) && canUse32BitIndexMath(output);

  // decide SIMD: SIMD32 or SIMD16
  auto* dev_prop = dpcppGetDeviceProperties(dpcppGetDeviceIdOfCurrentQueue());
  auto sub_group_size = dev_prop->subgroup_sizes;
  int SIMD = sub_group_size[1];
  if (SIMD == SIMD32) {
    if (dim_size < SIMD16 * INNER_LOOP)
      SIMD = SIMD16;
  }

#define DISPATCH_SOFTMAX_FORWARD_IMPL(vec_size, SIMD, outer_loop) \
  {                                                               \
    dispatch_softmax_forward_kernel<                              \
        INNER_LOOP,                                               \
        vec_size,                                                 \
        SIMD,                                                     \
        scalar_t,                                                 \
        accscalar_t,                                              \
        uint32_t,                                                 \
        LogSoftMax,                                               \
        outer_loop>(                                              \
        input.data_ptr<scalar_t>(),                               \
        output.data_ptr<scalar_t>(),                              \
        dim_size,                                                 \
        outer_size);                                              \
  }

#define SOFTMAX_FORWARD_IMPL(vec_size, IndexType) \
  {                                               \
    softmax_forward_kernel<                       \
        vec_size,                                 \
        scalar_t,                                 \
        accscalar_t,                              \
        IndexType,                                \
        LogSoftMax>(                              \
        input.data_ptr<scalar_t>(),               \
        output.data_ptr<scalar_t>(),              \
        dim_size,                                 \
        outer_size);                              \
  }

#define SPATIAL_SOFTMAX_FORWARD_IMPL(vec_size, IndexType) \
  {                                                       \
    spatial_softmax_forward<                              \
        vec_size,                                         \
        scalar_t,                                         \
        accscalar_t,                                      \
        IndexType,                                        \
        LogSoftMax>(                                      \
        input.data_ptr<scalar_t>(),                       \
        output.data_ptr<scalar_t>(),                      \
        dim_size,                                         \
        inner_size,                                       \
        outer_size);                                      \
  }

  if (inner_size == 1) {
    // if the element number is smaller than max_work_group_size * INNER_LOOP,
    // the fast path (dispatch_softmax_forward) will be selected.
    // otherwise, the general path (softmax_forward_kernel) will be selected.
    auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
    int max_group_size = dpcppMaxWorkGroupSize(dev_id);
    if (can_use_32bit_index && max_group_size * INNER_LOOP >= dim_size) {
      // it assumes vec_size * outer_loop * work_group_size >= dim_size

      if (SIMD == SIMD32) {
        // Ensure input/output tensor are aligned with max_vec_size
        if (input_start == 0 && output_start == 0 &&
            dim_size % max_vec_size == 0) {
          constexpr int outer_loop = INNER_LOOP / max_vec_size;
          DISPATCH_SOFTMAX_FORWARD_IMPL(
              /*vec_size*/ max_vec_size, /*SIMD*/ SIMD32, outer_loop);
        } else {
          constexpr int outer_loop = INNER_LOOP;
          DISPATCH_SOFTMAX_FORWARD_IMPL(
              /*vec_size*/ 1, /*SIMD*/ SIMD32, outer_loop);
        }
      } else {
        if (input_start == 0 && output_start == 0 &&
            dim_size % max_vec_size == 0) {
          if (max_vec_size >= 4 && dim_size <= 4 * SIMD) {
            // if vec_size >= 4 and dim_size <= 4 * SIMD, take smaller vec_size
            // and 1 outer_loop
            constexpr int outer_loop = 1;
            DISPATCH_SOFTMAX_FORWARD_IMPL(
                /*vec_size*/ 4, /*SIMD*/ SIMD16, outer_loop);
          } else if (dim_size <= max_vec_size * SIMD) {
            // if dim_size <= max_vec_size * SIMD , take 1 outer_loop
            constexpr int outer_loop = 1;
            DISPATCH_SOFTMAX_FORWARD_IMPL(
                /*vec_size*/ max_vec_size, /*SIMD*/ SIMD16, outer_loop);
          } else {
            // SIMD16 will use less register numbers than SIMD32
            // if the SIMD = SIMD16, then outer_loop will be enlarged 2x
            constexpr int outer_loop = INNER_LOOP / max_vec_size * 2;
            DISPATCH_SOFTMAX_FORWARD_IMPL(
                /*vec_size*/ max_vec_size, /*SIMD*/ SIMD16, outer_loop);
          }
        } else {
          constexpr int outer_loop = INNER_LOOP * 2;
          DISPATCH_SOFTMAX_FORWARD_IMPL(
              /*vec_size*/ 1, /*SIMD*/ SIMD16, outer_loop);
        }
      }
    } else {
      if (can_use_32bit_index) {
        // the start psition of tensor pointer should be the same
        // the kernel can handle the non-aligned status
        if (input_start == output_start) {
          SOFTMAX_FORWARD_IMPL(
              /*vec_size*/ max_vec_size, /*IndexType*/ uint32_t);
        } else {
          SOFTMAX_FORWARD_IMPL(/*vec_size*/ 1, /*IndexType*/ uint32_t);
        }
      } else {
        if (input_start == output_start) {
          SOFTMAX_FORWARD_IMPL(
              /*vec_size*/ max_vec_size, /*IndexType*/ uint64_t);
        } else {
          SOFTMAX_FORWARD_IMPL(/*vec_size*/ 1, /*IndexType*/ uint64_t);
        }
      }
    }
  } else {
    if (can_use_32bit_index) {
      if (input_start == output_start && inner_size % max_vec_size == 0) {
        SPATIAL_SOFTMAX_FORWARD_IMPL(
            /*vec_size*/ max_vec_size, /*IndexType*/ uint32_t);
      } else {
        SPATIAL_SOFTMAX_FORWARD_IMPL(/*vec_size*/ 1, /*IndexType*/ uint32_t);
      }
    } else {
      if (input_start == output_start && inner_size % max_vec_size == 0) {
        SPATIAL_SOFTMAX_FORWARD_IMPL(
            /*vec_size*/ max_vec_size, /*IndexType*/ uint64_t);
      } else {
        SPATIAL_SOFTMAX_FORWARD_IMPL(/*vec_size*/ 1, /*IndexType*/ uint64_t);
      }
    }
  }
#undef DISPATCH_SOFTMAX_FORWARD_IMPL
#undef SOFTMAX_FORWARD_IMPL
#undef SPATIAL_SOFTMAX_FORWARD_IMPL
}

template <typename scalar_t, typename accscalar_t>
Tensor& add_view_softmax_impl(
    const Tensor& input,
    const Tensor& other,
    int64_t dim,
    const Scalar& alpha_scalar,
    Tensor& output,
    IntArrayRef sizes) {
  auto alpha = alpha_scalar.to<scalar_t>();
  auto view_output = input.view(sizes);
  auto inner_size = view_output.stride(dim);
  // decide indexing range: uint32_t (4GB) or uint64_t (>4GB)
  bool can_use_32bit_index =
      canUse32BitIndexMath(view_output) && canUse32BitIndexMath(output);
  auto dim_size = view_output.size(dim);
  auto outer_size = view_output.numel() / (inner_size * dim_size);
  auto other_outer_size = outer_size;

  constexpr int float4_size = sizeof(float) * 4;
  constexpr int max_vec_size = float4_size / sizeof(scalar_t);
  constexpr int INNER_LOOP = max_vec_size * 2;

  bool fuse_pattern = false;
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  int max_group_size = dpcppMaxWorkGroupSize(dev_id);
  if (inner_size == 1 && can_use_32bit_index &&
      max_group_size * INNER_LOOP >= dim_size)
    fuse_pattern = true;
  if (fuse_pattern) {
    Tensor add_output = output.view(input.sizes());
    auto iter = TensorIterator::binary_op(add_output, input, other);
    auto input_calc = make_input_offset_calculator<2>(iter);

    // decide vec_size: max_vec_size or 1
    using vec_t =
        at::native::Memory::aligned_vector_loop<scalar_t, max_vec_size>;
    constexpr int align_bytes = alignof(vec_t);
    int input_start =
        ((uint64_t)input.data_ptr()) % align_bytes / sizeof(scalar_t);
    int output_start =
        ((uint64_t)output.data_ptr()) % align_bytes / sizeof(scalar_t);

    // decide SIMD: SIMD32 or SIMD16
    auto* dev_prop = dpcppGetDeviceProperties(dpcppGetDeviceIdOfCurrentQueue());
    auto sub_group_size = dev_prop->subgroup_sizes;
    int SIMD = sub_group_size[1];
    if (SIMD == SIMD32) {
      if (dim_size < SIMD16 * INNER_LOOP)
        SIMD = SIMD16;
    }
    // fused kernel
#define DISPATCH_SOFTMAX_FORWARD_IMPL(vec_size, SIMD, outer_loop) \
  {                                                               \
    dispatch_softmax_forward_add_kernel<                          \
        INNER_LOOP,                                               \
        vec_size,                                                 \
        SIMD,                                                     \
        scalar_t,                                                 \
        accscalar_t,                                              \
        uint32_t,                                                 \
        false,                                                    \
        outer_loop>(                                              \
        input.data_ptr<scalar_t>(),                               \
        other.data_ptr<scalar_t>(),                               \
        output.data_ptr<scalar_t>(),                              \
        dim_size,                                                 \
        alpha,                                                    \
        outer_size,                                               \
        other_outer_size,                                         \
        input_calc);                                              \
  }

    // if the element number is smaller than max_work_group_size *
    // INNER_LOOP, the fused path (dispatch_softmax_forward_add) will be
    // selected. otherwise, the general path (add then softmax) will be
    // selected.
    auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
    int max_group_size = dpcppMaxWorkGroupSize(dev_id);
    // it assumes vec_size * outer_loop * work_group_size >= dim_size
    if (SIMD == SIMD32) {
      // Ensure input/output tensor are aligned with max_vec_size
      if (input_start == 0 && output_start == 0 &&
          dim_size % max_vec_size == 0) {
        constexpr int outer_loop = INNER_LOOP / max_vec_size;
        DISPATCH_SOFTMAX_FORWARD_IMPL(
            /*vec_size*/ max_vec_size, /*SIMD*/ SIMD32, outer_loop);
      } else {
        constexpr int outer_loop = INNER_LOOP;
        DISPATCH_SOFTMAX_FORWARD_IMPL(
            /*vec_size*/ 1, /*SIMD*/ SIMD32, outer_loop);
      }
    } else {
      if (input_start == 0 && output_start == 0 &&
          dim_size % max_vec_size == 0) {
        if (max_vec_size >= 4 && dim_size <= 4 * SIMD) {
          // if vec_size >= 4 and dim_size <= 4 * SIMD, take smaller vec_size
          // and 1 outer_loop
          constexpr int outer_loop = 1;
          DISPATCH_SOFTMAX_FORWARD_IMPL(
              /*vec_size*/ 4, /*SIMD*/ SIMD16, outer_loop);
        } else if (dim_size <= max_vec_size * SIMD) {
          // if dim_size <= max_vec_size * SIMD , take 1 outer_loop
          constexpr int outer_loop = 1;
          DISPATCH_SOFTMAX_FORWARD_IMPL(
              /*vec_size*/ max_vec_size, /*SIMD*/ SIMD16, outer_loop);
        } else {
          // SIMD16 will use less register numbers than SIMD32
          // if the SIMD = SIMD16, then outer_loop will be enlarged 2x
          constexpr int outer_loop = INNER_LOOP / max_vec_size * 2;
          DISPATCH_SOFTMAX_FORWARD_IMPL(
              /*vec_size*/ max_vec_size, /*SIMD*/ SIMD16, outer_loop);
        }
      } else {
        constexpr int outer_loop = INNER_LOOP * 2;
        DISPATCH_SOFTMAX_FORWARD_IMPL(
            /*vec_size*/ 1, /*SIMD*/ SIMD16, outer_loop);
      }
    }
    return output;
#undef DISPATCH_SOFTMAX_FORWARD_IMPL
  } else {
    Tensor add_out = at::add(input, other, alpha).view(sizes);
    return at::softmax_out(output, add_out, dim);
  }
}

template <typename scalar_t, typename accscalar_t, bool LogSoftMax>
void SpatialSoftMaxBackward(
    Tensor& gradInput,
    Tensor& output,
    Tensor& gradOutput,
    int dim) {
  auto inner_size = output.stride(dim);
  auto dim_size = output.size(dim);
  auto outer_size = output.numel() / (dim_size * inner_size);

  constexpr int float4_size = sizeof(float) * 4;
  constexpr int max_vec_size = float4_size / sizeof(scalar_t);
  constexpr int INNER_LOOP = max_vec_size;

  // decide vec_size: max_vec_size or 1
  using vec_t = at::native::Memory::aligned_vector_loop<scalar_t, max_vec_size>;
  constexpr int align_bytes = alignof(vec_t);
  int gradin_start =
      ((uint64_t)gradInput.data_ptr()) % align_bytes / sizeof(scalar_t);
  int output_start =
      ((uint64_t)output.data_ptr()) % align_bytes / sizeof(scalar_t);
  int gradoutput_start =
      ((uint64_t)gradOutput.data_ptr()) % align_bytes / sizeof(scalar_t);

  // decide indexing range: uint32_t (4GB) or uint64_t (>4GB)
  bool can_use_32bit_index = canUse32BitIndexMath(gradInput) &&
      canUse32BitIndexMath(output) && canUse32BitIndexMath(gradOutput);

  // decide SIMD: SIMD32 or SIMD16
  auto* dev_prop = dpcppGetDeviceProperties(dpcppGetDeviceIdOfCurrentQueue());
  auto sub_group_size = dev_prop->subgroup_sizes;
  int SIMD = sub_group_size[1];
  if (SIMD == SIMD32) {
    if (dim_size < SIMD16 * max_vec_size)
      SIMD = SIMD16;
  }

#define DISPATCH_SOFTMAX_BACKWARD_IMPL(vec_size, SIMD) \
  {                                                    \
    dispatch_softmax_backward_kernel<                  \
        INNER_LOOP,                                    \
        vec_size,                                      \
        SIMD,                                          \
        scalar_t,                                      \
        accscalar_t,                                   \
        uint32_t,                                      \
        LogSoftMax>(                                   \
        gradInput.data_ptr<scalar_t>(),                \
        output.data_ptr<scalar_t>(),                   \
        gradOutput.data_ptr<scalar_t>(),               \
        dim_size,                                      \
        outer_size);                                   \
  }

#define SOFTMAX_BACKWARD_IMPL(vec_size, IndexType)                      \
  softmax_backward_kernel<vec_size, scalar_t, accscalar_t, LogSoftMax>( \
      gradInput.data_ptr<scalar_t>(),                                   \
      output.data_ptr<scalar_t>(),                                      \
      gradOutput.data_ptr<scalar_t>(),                                  \
      dim_size,                                                         \
      outer_size);

#define SPATIAL_SOFTMAX_BACKWARD_IMPL(vec_size, IndexType) \
  spatial_softmax_backward_kernel<                         \
      vec_size,                                            \
      scalar_t,                                            \
      accscalar_t,                                         \
      LogSoftMax>(                                         \
      gradInput.data_ptr<scalar_t>(),                      \
      output.data_ptr<scalar_t>(),                         \
      gradOutput.data_ptr<scalar_t>(),                     \
      dim_size,                                            \
      inner_size,                                          \
      outer_size);

  if (inner_size == 1) {
    auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
    int max_group_size = dpcppMaxWorkGroupSize(dev_id);
    // if the element number is smaller than max_work_group_size * INNER_LOOP
    // / 2, (2 indicates reading two tensors: output and gradOutput) the fast
    // path (dispatch_softmax_backward) will be selected. otherwise, the
    // general path (softmax_backward_kernel) will be selected.
    if (can_use_32bit_index && max_group_size * INNER_LOOP >= dim_size) {
      if (SIMD == SIMD32) {
        if (gradin_start == 0 && output_start == 0 && gradoutput_start == 0 &&
            dim_size % max_vec_size == 0) {
          DISPATCH_SOFTMAX_BACKWARD_IMPL(
              /*vec_size*/ max_vec_size, /*SIMD*/ SIMD32);
        } else {
          DISPATCH_SOFTMAX_BACKWARD_IMPL(/*vec_size*/ 1, /*SIMD*/ SIMD32);
        }
      } else {
        if (gradin_start == 0 && output_start == 0 && gradoutput_start == 0 &&
            dim_size % max_vec_size == 0) {
          DISPATCH_SOFTMAX_BACKWARD_IMPL(
              /*vec_size*/ max_vec_size, /*SIMD*/ SIMD16);
        } else {
          DISPATCH_SOFTMAX_BACKWARD_IMPL(/*vec_size*/ 1, /*SIMD*/ SIMD16);
        }
      }
    } else {
      if (can_use_32bit_index) {
        if (gradin_start == output_start && gradin_start == gradoutput_start) {
          SOFTMAX_BACKWARD_IMPL(
              /*vec_size*/ max_vec_size, /*IndexType*/ uint32_t);
        } else {
          SOFTMAX_BACKWARD_IMPL(/*vec_size*/ 1, /*IndexType*/ uint32_t);
        }
      } else {
        if (gradin_start == output_start && gradin_start == gradoutput_start) {
          SOFTMAX_BACKWARD_IMPL(
              /*vec_size*/ max_vec_size, /*IndexType*/ uint64_t);
        } else {
          SOFTMAX_BACKWARD_IMPL(/*vec_size*/ 1, /*IndexType*/ uint64_t);
        }
      }
    }
  } else {
    if (can_use_32bit_index) {
      if (gradin_start == output_start && gradin_start == gradoutput_start &&
          inner_size % max_vec_size == 0) {
        SPATIAL_SOFTMAX_BACKWARD_IMPL(
            /*vec_size*/ max_vec_size, /*IndexType*/ uint32_t);
      } else {
        SPATIAL_SOFTMAX_BACKWARD_IMPL(/*vec_size*/ 1, /*IndexType*/ uint32_t);
      }
    } else {
      if (gradin_start == output_start && gradin_start == gradoutput_start &&
          inner_size % max_vec_size == 0) {
        SPATIAL_SOFTMAX_BACKWARD_IMPL(
            /*vec_size*/ max_vec_size, /*IndexType*/ uint64_t);
      } else {
        SPATIAL_SOFTMAX_BACKWARD_IMPL(1, uint64_t);
      }
    }
  }
#undef DISPATCH_SOFTMAX_BACKWARD_IMPL
#undef SOFTMAX_BACKWARD_IMPL
#undef SPATIAL_SOFTMAX_BACKWARD_IMPL
}

template <typename scalar_t, typename accscalar_t, bool LogSoftMax>
void MaskedSoftMaxBackward(
    Tensor& gradInput,
    Tensor& output,
    Tensor& gradOutput,
    Tensor& mask,
    int dim) {
  auto inner_size = output.stride(dim);
  auto dim_size = output.size(dim);
  auto outer_size = output.numel() / (dim_size * inner_size);

  constexpr int float4_size = sizeof(float) * 4;
  constexpr int max_vec_size = float4_size / sizeof(scalar_t);
  constexpr int INNER_LOOP = max_vec_size;

  // decide vec_size: max_vec_size or 1
  using vec_t = at::native::Memory::aligned_vector_loop<scalar_t, max_vec_size>;
  constexpr int align_bytes = alignof(vec_t);
  int gradin_start =
      ((uint64_t)gradInput.data_ptr()) % align_bytes / sizeof(scalar_t);
  int output_start =
      ((uint64_t)output.data_ptr()) % align_bytes / sizeof(scalar_t);
  int gradoutput_start =
      ((uint64_t)gradOutput.data_ptr()) % align_bytes / sizeof(scalar_t);

  // decide indexing range: uint32_t (4GB) or uint64_t (>4GB)
  bool can_use_32bit_index = canUse32BitIndexMath(gradInput) &&
      canUse32BitIndexMath(output) && canUse32BitIndexMath(gradOutput);

  // decide SIMD: SIMD32 or SIMD16
  auto* dev_prop = dpcppGetDeviceProperties(dpcppGetDeviceIdOfCurrentQueue());
  auto sub_group_size = dev_prop->subgroup_sizes;
  int SIMD = sub_group_size[1];
  if (SIMD == SIMD32) {
    if (dim_size < SIMD16 * max_vec_size)
      SIMD = SIMD16;
  }

#define DISPATCH_MASK_SOFTMAX_BACKWARD_IMPL(vec_size, SIMD) \
  {                                                         \
    dispatch_softmax_backward_kernel<                       \
        INNER_LOOP,                                         \
        vec_size,                                           \
        SIMD,                                               \
        scalar_t,                                           \
        accscalar_t,                                        \
        uint32_t,                                           \
        LogSoftMax,                                         \
        true,                                               \
        decltype(input_calc)>(                              \
        gradInput.data_ptr<scalar_t>(),                     \
        output.data_ptr<scalar_t>(),                        \
        gradOutput.data_ptr<scalar_t>(),                    \
        dim_size,                                           \
        outer_size,                                         \
        mask.data_ptr<bool>(),                              \
        input_calc);                                        \
  }

  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();
  int max_group_size = dpcppMaxWorkGroupSize(dev_id);
  if (inner_size == 1 && can_use_32bit_index &&
      max_group_size * INNER_LOOP >= dim_size) {
    auto iter = TensorIterator::binary_op(gradInput, gradOutput, mask);
    auto input_calc = make_input_offset_calculator<2>(iter);
    // if the element number is smaller than max_work_group_size * INNER_LOOP
    // / 2, (2 indicates reading two tensors: output and gradOutput) the fast
    // path (dispatch_softmax_backward) will be selected. otherwise, the
    // general path (softmax_backward_kernel) will be selected.
    if (SIMD == SIMD32) {
      if (gradin_start == 0 && output_start == 0 && gradoutput_start == 0 &&
          dim_size % max_vec_size == 0) {
        DISPATCH_MASK_SOFTMAX_BACKWARD_IMPL(
            /*vec_size*/ max_vec_size, /*SIMD*/ SIMD32);
      } else {
        DISPATCH_MASK_SOFTMAX_BACKWARD_IMPL(/*vec_size*/ 1, /*SIMD*/ SIMD32);
      }
    } else {
      if (gradin_start == 0 && output_start == 0 && gradoutput_start == 0 &&
          dim_size % max_vec_size == 0) {
        DISPATCH_MASK_SOFTMAX_BACKWARD_IMPL(
            /*vec_size*/ max_vec_size, /*SIMD*/ SIMD16);
      } else {
        DISPATCH_MASK_SOFTMAX_BACKWARD_IMPL(/*vec_size*/ 1, /*SIMD*/ SIMD16);
      }
    }
  } else {
    gradInput = at::_softmax_backward_data_out(
        gradInput,
        gradOutput,
        output.masked_fill(mask, 0),
        dim,
        gradOutput.scalar_type());
  }
#undef DISPATCH_SOFTMAX_BACKWARD_IMPL
}

} // namespace impl

template <bool LogSoftMax>
Tensor host_softmax(
    const Tensor& input_,
    const int64_t dim_,
    const bool half_to_float,
    Tensor& output) {
  AT_ASSERTM(
      !half_to_float,
      "softmax with half to float conversion is not supported on XPU");
  TORCH_CHECK(
      input_.is_contiguous(),
      "** host_softmax only supports contiguous input tensor");
  if (!output.defined()) {
    output = at::native::empty_like(input_);
  }
  Tensor input = input_;
  if (input.dim() == 0)
    input = input.view(1);
  int64_t dim = maybe_wrap_dim(dim_, input.dim());
  TORCH_CHECK(
      dim >= 0 && dim < input.dim(),
      "** dpcpp dim must be non-negative and less than input dimensions");

  if (input.numel() > 0) {
    IPEX_DISPATCH_FLOATING_TYPES_AND2(
        at::ScalarType::BFloat16,
        at::ScalarType::Half,
        input.scalar_type(),
        "host_softmax",
        [&] {
          using accscalar_t = acc_type<scalar_t>;
          impl::SpatialSoftMaxForward<scalar_t, accscalar_t, LogSoftMax>(
              output, input, dim);
        });
  }
  return output;
}

bool shape_use_fused_path(const Tensor& input, const Tensor& other) {
  // for add_softmaxi_fusion, we support shapes like:
  // [N, C, H, W], [N1, C1, H1, W1] which X is divisible by X1
  // [N, C, H, W], [C1, H1, W1] which X is divisible by X1
  // [N, C, H, W], [H1, W1] which X is divisible by X1
  // [N, C, H, W], [W1] which X is divisible by X1
  // likewise for 3D and 5D inputs

  if (input.sizes() == other.sizes())
    return true;
  auto a_dim = input.dim();
  auto b_dim = other.dim();
  if (b_dim > a_dim)
    return false;
  auto input_size = input.sizes();
  auto other_size = other.sizes();
  // loop for the smaller shape from end
  for (int i = 1; i <= b_dim; i++) {
    if (input_size[a_dim - i] % other_size[b_dim - i] != 0) {
      return false;
    }
  }
  return true;
}

Tensor add_softmax(
    const Tensor& input,
    const Tensor& other,
    Scalar alpha,
    const int64_t dim,
    c10::optional<ScalarType> dtype) {
  RECORD_FUNCTION("torch_ipex::add_softmax", {});

  // fall back to no fuse path for different type inputs or not supported shapes
  if (!shape_use_fused_path(input, other) ||
      (input.scalar_type() != other.scalar_type()) ||
      (dtype.has_value() && (dtype.value() != input.scalar_type()))) {
    return at::softmax(at::add(input, other, alpha), dim, dtype);
  }
  IntArrayRef sizes;
  Tensor output;
  sizes = input.sizes();
  output = at::empty_like(input);
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::BFloat16,
      at::ScalarType::Half,
      input.scalar_type(),
      "add_softmax",
      [&] {
        using accscalar_t = acc_type<scalar_t>;
        impl::add_view_softmax_impl<scalar_t, accscalar_t>(
            input, other, dim, alpha, output, sizes);
      });
  return output;
}

Tensor add_view(
    const Tensor& input,
    const Tensor& other,
    Scalar alpha,
    IntArrayRef sizes) {
  return at::add(input, other, alpha).view(sizes);
}

Tensor add_scalar_view(
    const Tensor& input,
    Scalar other,
    Scalar alpha,
    IntArrayRef sizes) {
  return at::add(input, other, alpha).view(sizes);
}

Tensor add_view_softmax(
    const Tensor& input,
    const Tensor& other,
    Scalar alpha,
    IntArrayRef sizes,
    const int64_t dim,
    c10::optional<ScalarType> dtype) {
  RECORD_FUNCTION("torch_ipex::add_view_softmax", {});
  // fall back to no fuse path for different type inputs or not supported shapes

  if (!shape_use_fused_path(input, other) ||
      (input.scalar_type() != other.scalar_type()) ||
      (dtype.has_value() && dtype.value() != input.scalar_type())) {
    return at::softmax(at::add(input, other, alpha).view(sizes), dim, dtype);
  }

  Tensor output = at::empty_like(input).view(sizes);

  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::BFloat16,
      at::ScalarType::Half,
      input.scalar_type(),
      "add_view_softmax",
      [&] {
        using accscalar_t = acc_type<scalar_t>;
        impl::add_view_softmax_impl<scalar_t, accscalar_t>(
            input, other, dim, alpha, output, sizes);
      });
  return output;
}

template <bool LogSoftMax>
Tensor host_softmax_backward(
    const Tensor& grad_,
    const Tensor& output_,
    int64_t dim_,
    bool half_to_float,
    Tensor& gI) {
  AT_ASSERTM(
      !half_to_float,
      "softmax with half to float conversion is not supported on XPU");
  TORCH_CHECK(
      grad_.is_contiguous(),
      "** host_softmax_backward only supports contiguous grad tensor");
  TORCH_CHECK(
      output_.is_contiguous(),
      "** host_softmax_backward only supports contiguous output tensor");

  int64_t dim = maybe_wrap_dim(dim_, grad_.dim());
  if (!gI.defined()) {
    gI = at::empty_like(grad_);
  }

  if (output_.numel() == 0) {
    return gI;
  }

  Tensor grad = grad_;
  if (grad.dim() == 0)
    grad = grad.view(1);
  TORCH_CHECK(
      dim >= 0 && dim < grad.dim(),
      "dim must be non-negative and less than input dimensions");
  Tensor output = output_;
  if (output.dim() == 0)
    output = output.view(1);
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::BFloat16,
      at::ScalarType::Half,
      grad.scalar_type(),
      "host_softmax_backward",
      [&] {
        using accscalar_t = acc_type<scalar_t>;
        impl::SpatialSoftMaxBackward<scalar_t, accscalar_t, LogSoftMax>(
            gI, output, grad, dim);
      });
  return gI;
}

// We now use SYCL softmax fwd kernel instead of oneDNN softmax fwd kernel
Tensor& _softmax_out(
    const Tensor& input_,
    int64_t dim,
    bool half_to_float,
    Tensor& out) {
  checkBackend("_softmax", {input_}, Backend::XPU);

  xpu::COMPUTE_ENG real_eng;
  bool is_softmax_valid = xpu::oneDNN::softmax_valid(input_);
  if (!is_softmax_valid) {
    real_eng = xpu::COMPUTE_ENG::BASIC;
  } else {
    real_eng = choose_compute_eng(xpu::COMPUTE_ENG::BASIC, input_);
  }

  // 1.check the tensors type are supported by oneDNN or not
  // 2.check the tensors are contiguous or not
  // 3.check the tensors are blocked format or not
  // when satify the aformentioned two conditions,
  // the oneDNN path will be selected,
  // all the other cases will go to SYCL path
  if (xpu::COMPUTE_ENG::ONEDNN == real_eng) {
    xpu::oneDNN::softmax(input_, dim, half_to_float, out);
    return out;
  } else {
    Tensor input = to_plain_if_needed(input_).contiguous();
    host_softmax<false>(input, dim, half_to_float, out);
    return out;
  }
}

Tensor& _softmax_backward_data_out(
    const Tensor& grad_output,
    const Tensor& output,
    int64_t dim,
    at::ScalarType input_dtype,
    Tensor& grad_input) {
  bool half_to_float = grad_output.scalar_type() != input_dtype;
  if (half_to_float) {
    TORCH_CHECK(
        !half_to_float,
        "softmax backward with half to float "
        "conversion is not supported on XPU");
  }

  // 1.check the tensors type are supported by oneDNN or not
  // 2.check the tensors are contiguous or not
  // 3.check the tensors are blocked format or not
  // when satify the aformentioned conditions,
  // the oneDNN path will be selected,
  // all the other cases will go to SYCL path
  if (xpu::oneDNN::softmax_backward_valid(grad_output, output, grad_input) &&
      IPEX_ANY(xpu::oneDNN::is_onednn_layout, grad_output, output)) {
    xpu::oneDNN::softmax_backward(
        grad_output, output, dim, half_to_float, grad_input);
    return grad_input;
  } else {
    auto grad_ = to_plain_if_needed(grad_output).contiguous();
    auto output_ = to_plain_if_needed(output).contiguous();
    host_softmax_backward<false>(
        grad_, output_, dim, half_to_float, grad_input);
    return grad_input;
  }
}

at::Tensor& _log_softmax_out(
    const at::Tensor& self,
    int64_t dim,
    bool half_to_float,
    at::Tensor& out) {
  Tensor self_ = self.contiguous();
  host_softmax<true>(self_, dim, half_to_float, out);
  return out;
}

at::Tensor& _log_softmax_backward_data_out(
    const at::Tensor& grad_output,
    const at::Tensor& output,
    int64_t dim,
    at::ScalarType input_dtype,
    at::Tensor& out) {
  bool half_to_float = grad_output.scalar_type() != input_dtype;
  if (half_to_float) {
    TORCH_INTERNAL_ASSERT(
        !half_to_float,
        "softmax with half to float conversion is not supported on XPU");
  }

  auto grad_ = grad_output.contiguous();
  auto output_ = output.contiguous();
  host_softmax_backward<true>(grad_, output_, dim, half_to_float, out);
  return out;
}

Tensor _masked_softmax(
    const Tensor& input_,
    const Tensor& mask_,
    const c10::optional<int64_t> dim_,
    const c10::optional<int64_t> mask_type_) {
  Tensor output = at::empty_like(input_, input_.options());
  TORCH_CHECK(
      mask_.scalar_type() == ScalarType::Bool,
      "Mask should be a boolean tensor");

  TORCH_CHECK(mask_type_.has_value(), "Mask Type should be defined");
  int64_t mask_type = mask_type_.value();
  TORCH_CHECK(
      (mask_type == 0) || (mask_type == 1) || (mask_type == 2),
      "Mask Type should be 0 (src_mask), 1 (src_key_padding_mask), or 2 (default_mask)");

  // If input is [B, H, T, T] and mask is [B, T]
  // we have special fast kernel
  // mask_type == 1 => mask_ is a src_key_padding_mask
  bool is_BxT_mask = (mask_type == 1) &&
      (input_.dim() == 4 && mask_.dim() == 2 &&
       input_.size(0) == mask_.size(0) && input_.size(2) == mask_.size(1) &&
       input_.size(3) == mask_.size(1));

  // If input is [B, H, T, T] and mask is [T, T]
  // expand mask to [B, H, T, T] and treat it like regular mask
  // TODO We should have special fast kernel for TxT mask as well
  // mask_type == 0 => mask_ is a src_mask
  bool is_TxT_mask = (mask_type == 0) && input_.dim() == 4 &&
      mask_.dim() == 2 && input_.size(3) == mask_.size(1) &&
      input_.size(2) == mask_.size(0) && mask_.size(0) == mask_.size(1);
  // If mask_type == 2, then mask_.sizes() must equal input_.sizes()
  TORCH_CHECK(
      mask_.sizes() == input_.sizes() || is_BxT_mask || is_TxT_mask,
      "Mask shape should match input. mask: ",
      mask_.sizes(),
      " input: ",
      input_.sizes());

  auto input = input_.dim() == 0 ? input_.view(1) : input_;
  auto mask = mask_.dim() == 0 ? mask_.view(1) : mask_;
  int64_t dim = dim_.has_value() ? dim_.value() : input.dim() - 1;

  if (is_BxT_mask) {
    mask = mask.view({mask_.size(0), 1, 1, mask_.size(1)});
  }
  // Here assumes that the mask is broadcastable for input
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      ScalarType::Half,
      ScalarType::BFloat16,
      input.scalar_type(),
      "masked_softmax",
      [&] {
        using accscalar_t = acc_type<scalar_t>;
        impl::MaskedSoftMaxForward<scalar_t, accscalar_t, false>(
            output, input, dim, mask);
      });
  return output;
}

Tensor _masked_softmax_backward(
    const Tensor& grad_,
    const Tensor& output_,
    const Tensor& mask_,
    const c10::optional<int64_t> dim_) {
  Tensor grad_input = at::empty_like(grad_, grad_.options());
  if (grad_.numel() == 0) {
    return grad_input;
  }

  auto grad = grad_.contiguous();
  auto output = output_.contiguous();
  auto mask = mask_.contiguous();
  int64_t dim = dim_.has_value() ? maybe_wrap_dim(dim_.value(), output.dim())
                                 : output.dim() - 1;

  grad = grad.dim() == 0 ? grad.view(1) : grad;
  mask = mask.dim() == 0 ? mask.view(1) : mask;
  output = output.dim() == 0 ? output.view(1) : output;

  TORCH_CHECK(
      dim >= 0 && dim < grad.dim(),
      "dim must be non-negative and less than input dimensions");
  TORCH_CHECK(
      grad.sizes() == mask.sizes(), "Mask shape should match grad shape");
  TORCH_CHECK(
      mask.scalar_type() == ScalarType::Bool,
      "Mask should be a boolean tensor");

  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      ScalarType::Half,
      ScalarType::BFloat16,
      grad_input.scalar_type(),
      "masked_softmax_backward",
      [&] {
        using accscalar_t = acc_type<scalar_t>;
        impl::MaskedSoftMaxBackward<scalar_t, accscalar_t, false>(
            grad_input, output, grad, mask, dim);
      });
  return grad_input;
}

} // namespace AtenIpexTypeXPU
} // namespace at

namespace {
TORCH_LIBRARY_FRAGMENT(torch_ipex, m) {
  IPEX_OP_REGISTER("add_softmax", at::AtenIpexTypeXPU::add_softmax);
  IPEX_OP_REGISTER("add_view", at::AtenIpexTypeXPU::add_view);
  IPEX_OP_REGISTER("add_view.Scalar", at::AtenIpexTypeXPU::add_scalar_view);
  IPEX_OP_REGISTER("add_view_softmax", at::AtenIpexTypeXPU::add_view_softmax);
}

} // namespace
