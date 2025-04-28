// Copyright (C) 2023-2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <numeric>
#include <memory>
#include <sstream>

#include "gguf_utils/gguf.hpp"

using namespace std;

void unpack_32_4(uint8_t* data, uint8_t* dst) {
  std::fill_n(dst, 16, 0);
  for (int j = 0; j < 16; ++j) {
    uint8_t x = (data[j + 2] & 0x0F); // j+2 to skip scale bytes.
    if (j % 2 != 0) {
      x <<= 4;
    }
    dst[j / 2] |= x;
  }
  // Last 16 weights are in the higher bits
  for (int j = 0; j < 16; ++j) {
    uint8_t x = (data[j + 2] >> 4);
    if (j % 2 != 0) {
      x <<= 4;
    }
    dst[8 + j / 2] |= x;
  }
}

// Extracts (weight, scales, biases) from Q4_0 tensors.
// Data layout is: |16 bit scale|32 x 4bit weights|.
void extract_q4_0_data(
    const gguf_tensor& tensor,
    ov::Tensor& weights_arr,
    ov::Tensor& scales_arr,
    ov::Tensor& biases_arr) {
  const uint64_t bytes_per_block = 18; // 2 bytes scale, 32x0.5 byte weights
  auto data = static_cast<uint8_t*>(tensor.weights_data);
  auto weights = static_cast<uint8_t*>(weights_arr.data());
  auto scales = scales_arr.data<ov::element_type_traits<ov::element::f16>::value_type>();
  auto biases = biases_arr.data<ov::element_type_traits<ov::element::f16>::value_type>();
  for (int64_t i = 0; i < scales_arr.get_size(); i++) {
    scales[i] = ov::float16::from_bits(*((uint16_t*)data));
    biases[i] = ov::float16(-8.f * static_cast<float>(scales[i]));
    unpack_32_4(data, weights);
    weights += 16;
    data += bytes_per_block;
  }
}

// Extracts (weight, scales, biases) from Q4_1 tensors.
// Data layout is: |16 bit scale|16 bit bias|32 x 4bit weights|.
void extract_q4_1_data(
    const gguf_tensor& tensor,
    ov::Tensor& weights_arr,
    ov::Tensor& scales_arr,
    ov::Tensor& biases_arr) {
  const uint64_t bytes_per_block =
      20; // 2 bytes scale, 2 bytes bias, 32x0.5 byte weights
  auto data = static_cast<uint8_t*>(tensor.weights_data);
  auto weights = static_cast<uint8_t*>(weights_arr.data());
  auto scales = scales_arr.data<ov::element_type_traits<ov::element::f16>::value_type>();
  auto biases = biases_arr.data<ov::element_type_traits<ov::element::f16>::value_type>();
  for (int64_t i = 0; i < scales_arr.get_size(); i++) {
    scales[i] = ov::float16::from_bits(*((uint16_t*)data));
    biases[i] = ov::float16::from_bits(*((uint16_t*)data + 1));
    unpack_32_4(data, weights);
    weights += 16;
    data += bytes_per_block;
  }
}

// Extracts (weight, scales, biases) from Q8_0 tensors.
// Data layout is: |16 bit scale|32 x 8bit weights|.
void extract_q8_0_data(
    const gguf_tensor& tensor,
    ov::Tensor& weights_arr,
    ov::Tensor& scales_arr,
    ov::Tensor& biases_arr) {
  const uint64_t weights_per_block = 32;
  const uint64_t bytes_per_block = 34; // 2 bytes scale, 32x1 byte weights
  auto data = static_cast<uint8_t*>(tensor.weights_data);
  auto weights = static_cast<uint8_t*>(weights_arr.data());
  auto scales = scales_arr.data<ov::element_type_traits<ov::element::f16>::value_type>();
  auto biases = biases_arr.data<ov::element_type_traits<ov::element::f16>::value_type>();
  for (int64_t i = 0; i < scales_arr.get_size(); i++) {
    uint8_t* block_data = data + i * bytes_per_block;
    scales[i] = ov::float16::from_bits(*(uint16_t*)block_data);
    biases[i] = ov::float16(-128.f * static_cast<float>(scales[i]));
    for (int64_t j = 0; j < weights_per_block; ++j) {
      uint8_t x = block_data[j + 2]; // j+2 to skip the scale bytes.
      // Original data is in int8_t, so we add a bias of -128 and invert the
      // first bit.
      x ^= 1 << 7;
      weights[i * weights_per_block + j] = x;
    }
  }
}

void gguf_load_quantized(
    std::unordered_map<std::string, ov::Tensor>& a,
    const gguf_tensor& tensor) {
  uint64_t weights_per_byte;
  if (tensor.type == GGUF_TYPE_Q4_0 || tensor.type == GGUF_TYPE_Q4_1) {
    weights_per_byte = 2;
  } else { // tensor.type == GGUF_TYPE_Q8_0
    weights_per_byte = 1;
  }

  std::string name(tensor.name, tensor.namelen);

  auto shape = get_shape(tensor);
  const uint64_t weights_per_block = 32;
  OPENVINO_ASSERT(shape.back() % weights_per_block == 0, "[load_gguf] tensor ", name, " has incompatible last dim shape: ", shape.back());

  auto weights_shape = shape;
  weights_shape.back() /= (weights_per_byte * 4);
  auto w_nbytes = sizeof(uint32_t) *
      std::accumulate(weights_shape.begin(),
                      weights_shape.end(),
                      1,
                      std::multiplies<size_t>());

  ov::Tensor weights(ov::element::u32, std::move(weights_shape));

  // For scales and bias
  shape[shape.size() - 1] = shape[shape.size() - 1] / weights_per_block;
  auto sb_nbytes = ov::element::f16.size() *
      std::accumulate(shape.begin(), shape.end(), 1, std::multiplies<size_t>());

  ov::Tensor scales(ov::element::f16, std::move(shape));
  ov::Tensor biases(ov::element::f16, std::move(shape));
  if (tensor.type == GGUF_TYPE_Q4_0) {
    extract_q4_0_data(tensor, weights, scales, biases);
  } else if (tensor.type == GGUF_TYPE_Q4_1) {
    extract_q4_1_data(tensor, weights, scales, biases);
  } else if (tensor.type == GGUF_TYPE_Q8_0) {
    extract_q8_0_data(tensor, weights, scales, biases);
  } else {
    OPENVINO_ASSERT("Unsupported tensor type in 'gguf_load_quantized'");
  }

  a.emplace(name, std::move(weights));

  auto check_insert = [](const auto& inserted) {
    OPENVINO_ASSERT(inserted.second, "[load_gguf] Duplicate parameter name ", inserted.first->second.data(),". This can happend when loading quantized tensors");
  };

  constexpr std::string_view weight_suffix = ".weight";
  const std::string name_prefix = name.substr(0, name.length() - weight_suffix.length());
  check_insert(a.emplace(name_prefix + ".scales", std::move(scales)));
  check_insert(a.emplace(name_prefix + ".biases", std::move(biases)));
}
