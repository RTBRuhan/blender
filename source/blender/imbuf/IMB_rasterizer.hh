/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup imbuf
 *
 * Rasterizer to render triangles onto an image buffer.
 *
 * The implementation is template based and follows a (very limited) OpenGL pipeline.
 *
 * ## Basic usage
 *
 * In order to use it you have to define the data structure for a single vertex.
 *
 * \code{.cc}
 * struct VertexInput {
 *  float2 uv;
 * };
 * \endcode
 *
 * A vertex shader is required to transfer the vertices to actual coordinates in the image buffer.
 * The vertex shader will store vertex specific data in a VertexOutInterface.
 *
 * \code{.cc}
 * class MyVertexShader : public AbstractVertexShader<VertexInput, float> {
 *   public:
 *     float4x4 mat;
 *     void vertex(const VertexInputType &input, VertexOutputType *r_output) override
 *     {
 *       float2 coord = float2(mat * float3(input.uv[0], input.uv[1], 0.0));
 *       r_output->coord = coord * image_size;
 *       r_output->data = 1.0;
 *     }
 * };
 * \endcode
 *
 * A fragment shader is required to actually generate the pixel that will be stored in the buffer.
 *
 * \code{.cc}
 * class FragmentShader : public AbstractFragmentShader<float, float4> {
 *   public:
 *     void fragment(const FragmentInputType &input, FragmentOutputType *r_output) override
 *     {
 *       *r_output = float4(input, input, input, 1.0);
 *     }
 * };
 * \endcode
 *
 * Create a rasterizer with the vertex and fragment shader and start drawing.
 * It is required to call flush to make sure that all triangles are drawn to the image buffer.
 *
 * \code{.cc}
 * Rasterizer<MyVertexShader, MyFragmentShader> rasterizer(&image_buffer);
 * rasterizer.get_vertex_shader().mat = float4x4::identity();
 * rasterizer.draw_triangle(
 *   VertexInput{float2(0.0, 1.0)},
 *   VertexInput{float2(1.0, 1.0)},
 *   VertexInput{float2(1.0, 0.0)},
 * );
 * rasterizer.flush();
 * \endcode
 */

#pragma once

#include "BLI_math.h"
#include "BLI_math_vec_types.hh"
#include "BLI_vector.hh"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "intern/rasterizer_clamping.hh"
#include "intern/rasterizer_stats.hh"

#include <optional>

#define DEBUG_PRINT

namespace blender::imbuf::rasterizer {

/** The default number of rasterlines to buffer before flushing to the image buffer. */
constexpr int64_t DefaultRasterlinesBufferSize = 4096;

/**
 * Interface data of the vertex stage.
 */
template<
    /**
     * Data type per vertex generated by the vertex shader and transferred to the fragment shader.
     *
     * The data type should implement the +=, =, -, / and * operator.
     */
    typename Inner>
class VertexOutInterface {
 public:
  using InnerType = Inner;
  using Self = VertexOutInterface<InnerType>;
  /** Coordinate of a vertex inside the image buffer. (0..image_buffer.x, 0..image_buffer.y). */
  float2 coord;
  InnerType data;

  Self &operator+=(const Self &other)
  {
    coord += other.coord;
    data += other.data;
    return *this;
  }

  Self &operator=(const Self &other)
  {
    coord = other.coord;
    data = other.data;
    return *this;
  }

  Self operator-(const Self &other) const
  {
    Self result;
    result.coord = coord - other.coord;
    result.data = data - other.data;
    return result;
  }

  Self operator/(const float divider) const
  {
    Self result;
    result.coord = coord / divider;
    result.data = data / divider;
    return result;
  }

  Self operator*(const float multiplier) const
  {
    Self result;
    result.coord = coord * multiplier;
    result.data = data * multiplier;
    return *this;
  }
};

/**
 * Vertex shader
 */
template<typename VertexInput, typename VertexOutput> class AbstractVertexShader {
 public:
  using VertexInputType = VertexInput;
  using VertexOutputType = VertexOutInterface<VertexOutput>;

  virtual void vertex(const VertexInputType &input, VertexOutputType *r_output) = 0;
};

/**
 * Fragment shader will render a single fragment onto the ImageBuffer.
 * FragmentInput - The input data from the vertex stage.
 * FragmentOutput points to the memory location to write to in the image buffer.
 */
template<typename FragmentInput, typename FragmentOutput> class AbstractFragmentShader {
 public:
  using FragmentInputType = FragmentInput;
  using FragmentOutputType = FragmentOutput;

  virtual void fragment(const FragmentInputType &input, FragmentOutputType *r_output) = 0;
};

/**
 * RasterLine - data to render a single rasterline of a triangle.
 */
template<typename FragmentInput> class Rasterline {
 public:
  /** Row where this rasterline will be rendered. */
  uint32_t y;
  /** Starting X coordinate of the rasterline. */
  uint32_t start_x;
  /** Ending X coordinate of the rasterline. */
  uint32_t end_x;
  /** Input data for the fragment shader on (start_x, y). */
  FragmentInput start_data;
  /** Delta to add to the start_input to create the data for the next fragment. */
  FragmentInput delta_step;

  Rasterline() = default;
  Rasterline(uint32_t y,
             uint32_t start_x,
             uint32_t end_x,
             FragmentInput start_data,
             FragmentInput delta_step)
      : y(y), start_x(start_x), end_x(end_x), start_data(start_data), delta_step(delta_step)
  {
  }
};

template<typename Rasterline, int64_t BufferSize> class Rasterlines {
 public:
  Vector<Rasterline> buffer;

  explicit Rasterlines() : buffer(BufferSize)
  {
  }

  void append(const Rasterline &value)
  {
    buffer.append(value);
  }

  bool is_empty() const
  {
    return buffer.is_empty();
  }

  bool has_items() const
  {
    return buffer.has_items();
  }

  bool is_full() const
  {
    return buffer.size() == BufferSize;
  }

  void clear()
  {
    buffer.clear();
  }
};

template<typename VertexShader,
         typename FragmentShader,

         /**
          * To improve branching performance the rasterlines are buffered and flushed when this
          * treshold is reached.
          */
         int64_t RasterlinesSize = DefaultRasterlinesBufferSize,

         /**
          * Statistic collector. Should be a subclass of AbstractStats or implement the same
          * interface.
          *
          * Is used in test cases to check what decision was made.
          */
         typename Statistics = NullStats>
class Rasterizer {
 public:
  using InterfaceInnerType = typename VertexShader::VertexOutputType::InnerType;
  using RasterlineType = Rasterline<InterfaceInnerType>;
  using VertexInputType = typename VertexShader::VertexInputType;
  using VertexOutputType = typename VertexShader::VertexOutputType;
  using FragmentInputType = typename FragmentShader::FragmentInputType;
  using FragmentOutputType = typename FragmentShader::FragmentOutputType;

  /** Check if the vertex shader and the fragment shader can be linked together. */
  static_assert(std::is_same_v<InterfaceInnerType, FragmentInputType>);

 private:
  VertexShader vertex_shader_;
  FragmentShader fragment_shader_;
  Rasterlines<RasterlineType, RasterlinesSize> rasterlines_;
  ImBuf *image_buffer_;

  const CenterPixelClampingMethod clamping_method;

 public:
  Statistics stats;

  explicit Rasterizer(ImBuf *image_buffer) : image_buffer_(image_buffer)
  {
  }

  virtual ~Rasterizer()
  {
    flush();
  }

  VertexShader &vertex_shader()
  {
    return vertex_shader_;
  }
  VertexShader &fragment_shader()
  {
    return fragment_shader_;
  }

  void draw_triangle(const VertexInputType &p1,
                     const VertexInputType &p2,
                     const VertexInputType &p3)
  {
    stats.increase_triangles();

    std::array<VertexOutputType, 3> vertex_out;

    vertex_shader_.vertex(p1, &vertex_out[0]);
    vertex_shader_.vertex(p2, &vertex_out[1]);
    vertex_shader_.vertex(p3, &vertex_out[2]);

    /* Early check if all coordinates are on a single of the buffer it is imposible to render into
     * the buffer*/
    const VertexOutputType &p1_out = vertex_out[0];
    const VertexOutputType &p2_out = vertex_out[1];
    const VertexOutputType &p3_out = vertex_out[2];
    const bool triangle_not_visible =
        (p1_out.coord[0] < 0.0 && p2_out.coord[0] < 0.0 && p3_out.coord[0] < 0.0) ||
        (p1_out.coord[1] < 0.0 && p2_out.coord[1] < 0.0 && p3_out.coord[1] < 0.0) ||
        (p1_out.coord[0] >= image_buffer_->x && p2_out.coord[0] >= image_buffer_->x &&
         p3_out.coord[0] >= image_buffer_->x) ||
        (p1_out.coord[1] >= image_buffer_->y && p2_out.coord[1] >= image_buffer_->y &&
         p3_out.coord[1] >= image_buffer_->y);
    if (triangle_not_visible) {
      stats.increase_discarded_triangles();
      return;
    }

    rasterize_triangle(vertex_out);
  }

  void flush()
  {
    if (rasterlines_.is_empty()) {
      return;
    }

    stats.increase_flushes();
    for (const RasterlineType &rasterline : rasterlines_.buffer) {
      render_rasterline(rasterline);
    }
    rasterlines_.clear();
  }

 private:
  void rasterize_triangle(std::array<VertexOutputType, 3> &vertex_out)
  {
#ifdef DEBUG_PRINT
    printf("%s 1: (%.4f,%.4f) 2: (%.4f,%.4f) 3: (%.4f %.4f)\n",
           __func__,
           vertex_out[0].coord[0],
           vertex_out[0].coord[1],
           vertex_out[1].coord[0],
           vertex_out[1].coord[1],
           vertex_out[2].coord[0],
           vertex_out[2].coord[1]);
#endif
    std::array<VertexOutputType *, 3> sorted_vertices = order_triangle_vertices(vertex_out);

    const int min_v = clamping_method.scanline_for(sorted_vertices[0]->coord[1]);
    const int mid_v = clamping_method.scanline_for(sorted_vertices[1]->coord[1]);
    const int max_v = clamping_method.scanline_for(sorted_vertices[2]->coord[1]) - 1;

    /* left and right branch. */
    VertexOutputType left = *sorted_vertices[0];
    VertexOutputType right = *sorted_vertices[0];

    VertexOutputType *left_target;
    VertexOutputType *right_target;
    if (sorted_vertices[1]->coord[0] < sorted_vertices[2]->coord[0]) {
      left_target = sorted_vertices[1];
      right_target = sorted_vertices[2];
    }
    else {
      left_target = sorted_vertices[2];
      right_target = sorted_vertices[1];
    }

    VertexOutputType left_add = calc_vertex_output_data(left, *left_target);
    VertexOutputType right_add = calc_vertex_output_data(right, *right_target);

    /* Change winding order to match the steepness of the edges. */
    if (right_add.coord[0] < left_add.coord[0]) {
      std::swap(left_add, right_add);
    }

    /* Perform a substep to make sure that the data of left and right match the data on the anchor
     * point (center of the pixel). */
    const float distance_to_minline_anchor_point = clamping_method.distance_to_scanline_anchor(
        sorted_vertices[0]->coord[1]);
    left += left_add * distance_to_minline_anchor_point;
    right += right_add * distance_to_minline_anchor_point;

    /* Add rasterlines from min_v to mid_v. */
    int v;
    for (v = min_v; v < mid_v; v++) {
      if (v >= 0 && v < image_buffer_->y) {
        std::optional<RasterlineType> rasterline = clamped_rasterline(
            v, left.coord[0], right.coord[0], left.data, right.data);
        if (rasterline) {
          append(*rasterline);
        }
      }
      left += left_add;
      right += right_add;
    }

    /* When both are the same we should the left/right branches are the same. */
    const float distance_to_midline_anchor_point = clamping_method.distance_to_scanline_anchor(
        sorted_vertices[1]->coord[1]);
    /* Use the x coordinate to identify which branch should be modified. */
    // TODO when min_v and mid_v are on the same scanline....
    const float distance_to_left = abs(left.coord[0] - sorted_vertices[1]->coord[0]);
    const float distance_to_right = abs(right.coord[0] - sorted_vertices[1]->coord[0]);
    if (distance_to_left < distance_to_right) {
      left = *sorted_vertices[1];
      left_target = sorted_vertices[2];
      left_add = calc_vertex_output_data(left, *left_target);
      left += left_add * distance_to_midline_anchor_point;
    }
    else {
      right = *sorted_vertices[1];
      right_target = sorted_vertices[2];
      right_add = calc_vertex_output_data(right, *right_target);
      right += right_add * distance_to_midline_anchor_point;
    }

    /* Add rasterlines from mid_v to max_v. */
    for (; v < max_v; v++) {
      if (v >= 0 && v < image_buffer_->y) {
        std::optional<RasterlineType> rasterline = clamped_rasterline(
            v, left.coord[0], right.coord[0], left.data, right.data);
        if (rasterline) {
          append(*rasterline);
        }
      }
      left += left_add;
      right += right_add;
    }
  }

  VertexOutputType calc_vertex_output_data(const VertexOutputType &from,
                                           const VertexOutputType &to)
  {
    const float num_rasterlines = to.coord[1] - from.coord[1];
    if (num_rasterlines == 0.0) {
      return (to - from);
    }
    return (to - from) / num_rasterlines;
  }

  std::array<VertexOutputType *, 3> order_triangle_vertices(
      std::array<VertexOutputType, 3> &vertex_out)
  {
    std::array<VertexOutputType *, 3> sorted;
    /* Find min v-coordinate and store at index 0. */
    sorted[0] = &vertex_out[0];
    for (int i = 1; i < 3; i++) {
      if (vertex_out[i].coord[1] < sorted[0]->coord[1]) {
        sorted[0] = &vertex_out[i];
      }
    }

    /* Find max v-coordinate and store at index 2. */
    sorted[2] = &vertex_out[0];
    for (int i = 1; i < 3; i++) {
      if (vertex_out[i].coord[1] > sorted[2]->coord[1]) {
        sorted[2] = &vertex_out[i];
      }
    }

    /* Exit when all 3 have the same v coordinate. Use the original input order. */
    if (sorted[0]->coord[1] == sorted[2]->coord[1]) {
      for (int i = 0; i < 3; i++) {
        sorted[i] = &vertex_out[i];
      }
      BLI_assert(sorted[0] != sorted[1] && sorted[0] != sorted[2] && sorted[1] != sorted[2]);
      return sorted;
    }

    /* Find mid v-coordinate and store at index 1. */
    sorted[1] = &vertex_out[0];
    for (int i = 0; i < 3; i++) {
      if (sorted[0] != &vertex_out[i] && sorted[2] != &vertex_out[i]) {
        sorted[1] = &vertex_out[i];
        break;
      }
    }

    BLI_assert(sorted[0] != sorted[1] && sorted[0] != sorted[2] && sorted[1] != sorted[2]);
    BLI_assert(sorted[0]->coord[1] <= sorted[1]->coord[1]);
    BLI_assert(sorted[0]->coord[1] < sorted[2]->coord[1]);
    BLI_assert(sorted[1]->coord[1] <= sorted[2]->coord[1]);
    return sorted;
  }

  std::optional<RasterlineType> clamped_rasterline(int32_t y,
                                                   float start_x,
                                                   float end_x,
                                                   InterfaceInnerType start_data,
                                                   InterfaceInnerType end_data)
  {
    BLI_assert(y >= 0 && y < image_buffer_->y);

    stats.increase_rasterlines();
    if (start_x >= end_x) {
      stats.increase_discarded_rasterlines();
      return std::nullopt;
    }
    if (end_x < 0) {
      stats.increase_discarded_rasterlines();
      return std::nullopt;
    }
    if (start_x >= image_buffer_->x) {
      stats.increase_discarded_rasterlines();
      return std::nullopt;
    }

    FragmentInputType add_x = (end_data - start_data) / (end_x - start_x);
    /* Is created rasterline clamped and should be added to the statistics. */
    bool is_clamped = false;

    /* Clamp the start_x to the first visible column anchor. */
    int32_t start_xi = clamping_method.column_for(start_x);
    float delta_to_anchor = clamping_method.distance_to_column_anchor(start_x);
    if (start_xi < 0) {
      delta_to_anchor += -start_xi;
      start_xi = 0;
      is_clamped = true;
    }
    start_data += add_x * delta_to_anchor;

    uint32_t end_xi = clamping_method.column_for(end_x);
    if (end_xi > image_buffer_->x) {
      end_xi = image_buffer_->x;
      is_clamped = true;
    }

    if (is_clamped) {
      stats.increase_clamped_rasterlines();
    }

#ifdef DEBUG_PRINT
    printf("%s y(%d) x(%d-%u)\n", __func__, y, start_xi, end_xi);
#endif

    return RasterlineType(y, (uint32_t)start_xi, end_xi, start_data, add_x);
  }

  void render_rasterline(const RasterlineType &rasterline)
  {
    FragmentInputType data = rasterline.start_data;
    for (uint32_t x = rasterline.start_x; x < rasterline.end_x; x++) {
      uint32_t pixel_index = (rasterline.y * image_buffer_->x + x);
      float *pixel_ptr = &image_buffer_->rect_float[pixel_index * 4];

      FragmentOutputType fragment_out;
      fragment_shader_.fragment(data, &fragment_out);
      copy_v4_v4(pixel_ptr, &fragment_out[0]);

      data += rasterline.delta_step;
    }
  }

  void append(const RasterlineType &rasterline)
  {
    rasterlines_.append(rasterline);
    if (rasterlines_.is_full()) {
      flush();
    }
  }
};

}  // namespace blender::imbuf::rasterizer