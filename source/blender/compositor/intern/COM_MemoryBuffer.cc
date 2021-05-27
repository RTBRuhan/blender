/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2011, Blender Foundation.
 */

#include "COM_MemoryBuffer.h"

#include "MEM_guardedalloc.h"

namespace blender::compositor {

MemoryBuffer::MemoryBuffer(MemoryProxy *memoryProxy, const rcti &rect, MemoryBufferState state)
{
  m_rect = rect;
  this->m_is_a_single_elem = false;
  this->m_memoryProxy = memoryProxy;
  this->m_num_channels = COM_data_type_num_channels(memoryProxy->getDataType());
  this->m_buffer = (float *)MEM_mallocN_aligned(
      sizeof(float) * buffer_len() * this->m_num_channels, 16, "COM_MemoryBuffer");
  is_owner_ = true;
  this->m_state = state;
  this->m_datatype = memoryProxy->getDataType();

  set_strides();
}

MemoryBuffer::MemoryBuffer(DataType dataType, const rcti &rect, bool is_a_single_elem)
{
  m_rect = rect;
  this->m_is_a_single_elem = is_a_single_elem;
  this->m_memoryProxy = nullptr;
  this->m_num_channels = COM_data_type_num_channels(dataType);
  this->m_buffer = (float *)MEM_mallocN_aligned(
      sizeof(float) * buffer_len() * this->m_num_channels, 16, "COM_MemoryBuffer");
  is_owner_ = true;
  this->m_state = MemoryBufferState::Temporary;
  this->m_datatype = dataType;

  set_strides();
}

MemoryBuffer::MemoryBuffer(float *buffer,
                           const int num_channels,
                           const int width,
                           const int height,
                           const bool is_a_single_elem)
{
  BLI_rcti_init(&m_rect, 0, width, 0, height);
  m_is_a_single_elem = is_a_single_elem;
  m_memoryProxy = nullptr;
  m_num_channels = num_channels;
  m_datatype = COM_num_channels_data_type(num_channels);
  m_buffer = buffer;
  is_owner_ = false;
  m_state = MemoryBufferState::Temporary;

  set_strides();
}

MemoryBuffer::MemoryBuffer(const MemoryBuffer &src)
    : MemoryBuffer(src.m_datatype, src.m_rect, false)
{
  m_memoryProxy = src.m_memoryProxy;
  /* src may be single elem buffer */
  fill_from(src);
}

void MemoryBuffer::set_strides()
{
  if (m_is_a_single_elem) {
    this->elem_stride = 0;
    this->row_stride = 0;
  }
  else {
    this->elem_stride = m_num_channels;
    this->row_stride = getWidth() * m_num_channels;
  }
}

void MemoryBuffer::clear()
{
  memset(m_buffer, 0, buffer_len() * m_num_channels * sizeof(float));
}

float MemoryBuffer::get_max_value() const
{
  float result = this->m_buffer[0];
  const unsigned int size = this->buffer_len();
  unsigned int i;

  const float *fp_src = this->m_buffer;

  for (i = 0; i < size; i++, fp_src += this->m_num_channels) {
    float value = *fp_src;
    if (value > result) {
      result = value;
    }
  }

  return result;
}

float MemoryBuffer::get_max_value(const rcti &rect) const
{
  rcti rect_clamp;

  /* first clamp the rect by the bounds or we get un-initialized values */
  BLI_rcti_isect(&rect, &this->m_rect, &rect_clamp);

  if (!BLI_rcti_is_empty(&rect_clamp)) {
    MemoryBuffer temp_buffer(this->m_datatype, rect_clamp);
    temp_buffer.fill_from(*this);
    return temp_buffer.get_max_value();
  }

  BLI_assert(0);
  return 0.0f;
}

MemoryBuffer::~MemoryBuffer()
{
  if (this->m_buffer && is_owner_) {
    MEM_freeN(this->m_buffer);
    this->m_buffer = nullptr;
  }
}

void MemoryBuffer::fill_from(const MemoryBuffer &src)
{
  BLI_assert(!this->is_a_single_elem());

  unsigned int otherY;
  unsigned int minX = MAX2(this->m_rect.xmin, src.m_rect.xmin);
  unsigned int maxX = MIN2(this->m_rect.xmax, src.m_rect.xmax);
  unsigned int minY = MAX2(this->m_rect.ymin, src.m_rect.ymin);
  unsigned int maxY = MIN2(this->m_rect.ymax, src.m_rect.ymax);
  int offset;
  int otherOffset;

  for (otherY = minY; otherY < maxY; otherY++) {
    otherOffset = src.get_coords_offset(minX, otherY);
    offset = this->get_coords_offset(minX, otherY);
    memcpy(&this->m_buffer[offset],
           &src.m_buffer[otherOffset],
           (maxX - minX) * this->m_num_channels * sizeof(float));
  }
}

void MemoryBuffer::writePixel(int x, int y, const float color[4])
{
  if (x >= this->m_rect.xmin && x < this->m_rect.xmax && y >= this->m_rect.ymin &&
      y < this->m_rect.ymax) {
    const int offset = get_coords_offset(x, y);
    memcpy(&this->m_buffer[offset], color, sizeof(float) * this->m_num_channels);
  }
}

void MemoryBuffer::addPixel(int x, int y, const float color[4])
{
  if (x >= this->m_rect.xmin && x < this->m_rect.xmax && y >= this->m_rect.ymin &&
      y < this->m_rect.ymax) {
    const int offset = get_coords_offset(x, y);
    float *dst = &this->m_buffer[offset];
    const float *src = color;
    for (int i = 0; i < this->m_num_channels; i++, dst++, src++) {
      *dst += *src;
    }
  }
}

static void read_ewa_pixel_sampled(void *userdata, int x, int y, float result[4])
{
  MemoryBuffer *buffer = (MemoryBuffer *)userdata;
  buffer->read(result, x, y);
}

void MemoryBuffer::readEWA(float *result, const float uv[2], const float derivatives[2][2])
{
  if (m_is_a_single_elem) {
    memcpy(result, m_buffer, sizeof(float) * this->m_num_channels);
  }
  else {
    BLI_assert(this->m_datatype == DataType::Color);
    float inv_width = 1.0f / (float)this->getWidth(), inv_height = 1.0f / (float)this->getHeight();
    /* TODO(sergey): Render pipeline uses normalized coordinates and derivatives,
     * but compositor uses pixel space. For now let's just divide the values and
     * switch compositor to normalized space for EWA later.
     */
    float uv_normal[2] = {uv[0] * inv_width, uv[1] * inv_height};
    float du_normal[2] = {derivatives[0][0] * inv_width, derivatives[0][1] * inv_height};
    float dv_normal[2] = {derivatives[1][0] * inv_width, derivatives[1][1] * inv_height};

    BLI_ewa_filter(this->getWidth(),
                   this->getHeight(),
                   false,
                   true,
                   uv_normal,
                   du_normal,
                   dv_normal,
                   read_ewa_pixel_sampled,
                   this,
                   result);
  }
}

}  // namespace blender::compositor
