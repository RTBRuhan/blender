/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2022 Intel Corporation */

#ifdef WITH_ONEAPI

#  include "device/oneapi/device_impl.h"

#  include "util/debug.h"
#  include "util/log.h"

#  include "kernel/device/oneapi/kernel.h"

CCL_NAMESPACE_BEGIN

static void queue_error_cb(const char *message, void *user_ptr)
{
  if (user_ptr) {
    *((std::string *)user_ptr) = message;
  }
}

OneapiDevice::OneapiDevice(const DeviceInfo &info,
                           oneAPIDLLInterface &oneapi_dll_object,
                           Stats &stats,
                           Profiler &profiler)
    : Device(info, stats, profiler),
      device_queue(nullptr),
      texture_info(this, "__texture_info", MEM_GLOBAL),
      kg_memory(nullptr),
      kg_memory_device(nullptr),
      kg_memory_size(0),
      oneapi_dll(oneapi_dll_object)
{
  need_texture_info = false;

  (oneapi_dll.oneapi_set_error_cb)(queue_error_cb, &oneapi_error_string);

  // Oneapi calls should be initialised on this moment;
  assert(oneapi_dll.oneapi_create_queue != nullptr);

  bool is_finished_ok = (oneapi_dll.oneapi_create_queue)(device_queue, info.num);
  if (is_finished_ok == false) {
    set_error("oneAPI queue initialization error: got runtime exception \"" + oneapi_error_string +
              "\"");
  }
  else {
    VLOG(1) << "oneAPI queue has been successfully created for the device \"" << info.description
            << "\"";
    assert(device_queue);
  }

  size_t globals_segment_size;
  is_finished_ok = (oneapi_dll.oneapi_kernel_globals_size)(device_queue, globals_segment_size);
  if (is_finished_ok == false) {
    set_error("oneAPI constant memory initialization got runtime exception \"" +
              oneapi_error_string + "\"");
  }
  else {
    VLOG(1) << "Successfuly created global/constant memory segment (kernel globals object)";
  }

  kg_memory = (oneapi_dll.oneapi_usm_aligned_alloc_host)(device_queue, globals_segment_size, 16);
  (oneapi_dll.oneapi_usm_memset)(device_queue, kg_memory, 0, globals_segment_size);

  kg_memory_device = (oneapi_dll.oneapi_usm_alloc_device)(device_queue, globals_segment_size);

  kg_memory_size = globals_segment_size;
}

OneapiDevice::~OneapiDevice()
{
  texture_info.free();
  (oneapi_dll.oneapi_usm_free)(device_queue, kg_memory);
  (oneapi_dll.oneapi_usm_free)(device_queue, kg_memory_device);

  ConstMemMap::iterator mt;
  for (mt = m_const_mem_map.begin(); mt != m_const_mem_map.end(); mt++)
    delete mt->second;

  if (device_queue)
    (oneapi_dll.oneapi_free_queue)(device_queue);
}

bool OneapiDevice::check_peer_access(Device * /*peer_device*/)
{
  return false;
}

BVHLayoutMask OneapiDevice::get_bvh_layout_mask() const
{
  return BVH_LAYOUT_BVH2;
}

string OneapiDevice::load_kernels_message(const uint /*requested_features*/)
{
  return "Loading render kernels (may take a few dozens of minutes)";
}

bool OneapiDevice::load_kernels(const uint requested_features)
{
  assert(device_queue);
  // NOTE(sirgienko) oneAPI can support compilation of kernel code with sertain feature set
  // with specialisation constants, but it hasn't been implemented yet.
  (void)requested_features;

  bool is_finished_ok = (oneapi_dll.oneapi_trigger_runtime_compilation)(device_queue);
  if (is_finished_ok == false) {
    set_error("oneAPI kernel load: got runtime exception \"" + oneapi_error_string + "\"");
  }
  else {
    VLOG(1) << "Runtime compilation done for \"" << info.description << "\"";
    assert(device_queue);
  }
  return is_finished_ok;
}

void OneapiDevice::load_texture_info()
{
  if (need_texture_info) {
    need_texture_info = false;
    texture_info.copy_to_device();
  }
}

void OneapiDevice::generic_alloc(device_memory &mem)
{
  size_t memory_size = mem.memory_size();

  // TODO(sirgienko) In future, if scene doesn't fit into device memory, then
  // we can use USM host memory.
  // Because of the expected performance impact, implementation of this has had a low priority
  // and is not implemented yet.

  assert(device_queue);
  // NOTE(sirgienko) There are three types of Unified Shared Memory (USM) in oneAPI: host, device
  // and shared. For new project it maybe more beneficial to use USM shared memory, because it
  // provides automatic migration mechansim in order to allow to use the same pointer on host and
  // on device, without need to worry about explicit memory transfer operations. But for
  // Blender/Cycles this type of memory is not very suitable in current application architecture,
  // because Cycles already uses two different pointer for host activity and device activity, and
  // also has to perform all needed memory transfer operations. So, USM device memory
  // type has been used for oneAPI device in order to better fit in Cycles architecture.
  void *device_pointer = (oneapi_dll.oneapi_usm_alloc_device)(device_queue, memory_size);
  if (device_pointer == nullptr) {
    size_t max_memory_on_device = (oneapi_dll.oneapi_get_memcapacity)(device_queue);
    set_error("oneAPI kernel - device memory allocation error for " +
              string_human_readable_size(mem.memory_size()) +
              ", possibly caused by lack of available memory space on the device: " +
              string_human_readable_size(stats.mem_used) + " of " +
              string_human_readable_size(max_memory_on_device) + " is already allocated");
    return;
  }
  assert(device_pointer);

  mem.device_pointer = (ccl::device_ptr)device_pointer;
  mem.device_size = memory_size;

  stats.mem_alloc(memory_size);
}

void OneapiDevice::generic_copy_to(device_memory &mem)
{
  size_t memory_size = mem.memory_size();

  // copy operation from host shouldn't be requested if there is no memory allocated on host.
  assert(mem.host_pointer);
  assert(device_queue);
  (oneapi_dll.oneapi_usm_memcpy)(
      device_queue, (void *)mem.device_pointer, (void *)mem.host_pointer, memory_size);
}

SyclQueue *OneapiDevice::sycl_queue()
{
  return device_queue;
}

string OneapiDevice::oneapi_error_message()
{
  return string(oneapi_error_string.c_str());
}

oneAPIDLLInterface OneapiDevice::oneapi_dll_object()
{
  return oneapi_dll;
}

void *OneapiDevice::kernel_globals_device_pointer()
{
  return kg_memory_device;
}

void OneapiDevice::generic_free(device_memory &mem)
{
  assert(mem.device_pointer);
  stats.mem_free(mem.device_size);
  mem.device_size = 0;

  assert(device_queue);
  (oneapi_dll.oneapi_usm_free)(device_queue, (void *)mem.device_pointer);
  mem.device_pointer = 0;
}

void OneapiDevice::mem_alloc(device_memory &mem)
{
  if (mem.type == MEM_TEXTURE) {
    assert(!"mem_alloc not supported for textures.");
  }
  else if (mem.type == MEM_GLOBAL) {
    assert(!"mem_alloc not supported for global memory.");
  }
  else {
    if (mem.name) {
      VLOG(2) << "OneapiDevice::mem_alloc: \"" << mem.name << "\", "
              << string_human_readable_number(mem.memory_size()).c_str() << " bytes. ("
              << string_human_readable_size(mem.memory_size()).c_str() << ")";
    }
    generic_alloc(mem);
  }
}

void OneapiDevice::mem_copy_to(device_memory &mem)
{
  if (mem.name) {
    VLOG(2) << "OneapiDevice::mem_copy_to: \"" << mem.name << "\", "
            << string_human_readable_number(mem.memory_size()).c_str() << " bytes. ("
            << string_human_readable_size(mem.memory_size()).c_str() << ")";
  }

  if (mem.type == MEM_GLOBAL) {
    global_free(mem);
    global_alloc(mem);
  }
  else if (mem.type == MEM_TEXTURE) {
    tex_free((device_texture &)mem);
    tex_alloc((device_texture &)mem);
  }
  else {
    if (!mem.device_pointer)
      mem_alloc(mem);

    generic_copy_to(mem);
  }
}

void OneapiDevice::mem_copy_from(device_memory &mem, size_t y, size_t w, size_t h, size_t elem)
{
  if (mem.type == MEM_TEXTURE || mem.type == MEM_GLOBAL) {
    assert(!"mem_copy_from not supported for textures.");
  }
  else if (mem.host_pointer) {
    const size_t size = elem * w * h;
    const size_t offset = elem * y * w;

    if (mem.name) {
      VLOG(2) << "OneapiDevice::mem_copy_from: \"" << mem.name << "\" object of "
              << string_human_readable_number(mem.memory_size()).c_str() << " bytes. ("
              << string_human_readable_size(mem.memory_size()).c_str() << ") from offset "
              << offset << " data " << size << " bytes";
    }

    assert(device_queue);

    assert(size != 0);
    assert(mem.device_pointer);
    char *shifted_host = (char *)mem.host_pointer + offset;
    char *shifted_device = (char *)mem.device_pointer + offset;
    bool is_finished_ok =
        (oneapi_dll.oneapi_usm_memcpy)(device_queue, shifted_host, shifted_device, size);
    if (is_finished_ok == false) {
      set_error("oneAPI memory operation error: got runtime exception \"" + oneapi_error_string +
                "\"");
    }
  }
}

void OneapiDevice::mem_zero(device_memory &mem)
{
  if (mem.name) {
    VLOG(2) << "OneapiDevice::mem_zero: \"" << mem.name << "\", "
            << string_human_readable_number(mem.memory_size()).c_str() << " bytes. ("
            << string_human_readable_size(mem.memory_size()).c_str() << ")\n";
  }

  if (!mem.device_pointer) {
    mem_alloc(mem);
  }
  if (!mem.device_pointer) {
    return;
  }

  assert(device_queue);
  bool is_finished_ok = (oneapi_dll.oneapi_usm_memset)(device_queue,
                                                       (void *)mem.device_pointer,
                                                       0,
                                                       mem.memory_size());
  if (is_finished_ok == false) {
    set_error("oneAPI memory operation error: got runtime exception \"" + oneapi_error_string +
              "\"");
  }
}

void OneapiDevice::mem_free(device_memory &mem)
{
  if (mem.name) {
    VLOG(2) << "OneapiDevice::mem_free: \"" << mem.name << "\", "
            << string_human_readable_number(mem.device_size).c_str() << " bytes. ("
            << string_human_readable_size(mem.device_size).c_str() << ")\n";
  }

  if (mem.type == MEM_GLOBAL) {
    global_free(mem);
  }
  else if (mem.type == MEM_TEXTURE) {
    tex_free((device_texture &)mem);
  }
  else {
    generic_free(mem);
  }
}

device_ptr OneapiDevice::mem_alloc_sub_ptr(device_memory &mem, size_t offset, size_t /*size*/)
{
  return (device_ptr)(((char *)mem.device_pointer) + mem.memory_elements_size(offset));
}

void OneapiDevice::const_copy_to(const char *name, void *host, size_t size)
{
  assert(name);

  VLOG(2) << "OneapiDevice::const_copy_to \"" << name << "\" object "
          << string_human_readable_number(size).c_str() << " bytes. ("
          << string_human_readable_size(size).c_str() << ")";

  ConstMemMap::iterator i = m_const_mem_map.find(name);
  device_vector<uchar> *data;

  if (i == m_const_mem_map.end()) {
    data = new device_vector<uchar>(this, name, MEM_READ_ONLY);
    data->alloc(size);
    m_const_mem_map.insert(ConstMemMap::value_type(name, data));
  }
  else {
    data = i->second;
  }

  assert(data->memory_size() <= size);
  memcpy(data->data(), host, size);
  data->copy_to_device();

  (oneapi_dll.oneapi_set_global_memory)(
      device_queue, kg_memory, name, (void *)data->device_pointer);

  (oneapi_dll.oneapi_usm_memcpy)(device_queue, kg_memory_device, kg_memory, kg_memory_size);
}

void OneapiDevice::global_alloc(device_memory &mem)
{
  assert(mem.name);

  size_t size = mem.memory_size();
  VLOG(2) << "OneapiDevice::global_alloc \"" << mem.name << "\" object "
          << string_human_readable_number(size).c_str() << " bytes. ("
          << string_human_readable_size(size).c_str() << ")";

  generic_alloc(mem);
  generic_copy_to(mem);

  (oneapi_dll.oneapi_set_global_memory)(
      device_queue, kg_memory, mem.name, (void *)mem.device_pointer);

  (oneapi_dll.oneapi_usm_memcpy)(device_queue, kg_memory_device, kg_memory, kg_memory_size);
}

void OneapiDevice::global_free(device_memory &mem)
{
  if (mem.device_pointer) {
    generic_free(mem);
  }
}

void OneapiDevice::tex_alloc(device_texture &mem)
{
  generic_alloc(mem);
  generic_copy_to(mem);

  // Resize if needed. Also, in case of resize - allocate in advance for future allocs
  const uint slot = mem.slot;
  if (slot >= texture_info.size()) {
    texture_info.resize(slot + 128);
  }

  texture_info[slot] = mem.info;
  need_texture_info = true;

  texture_info[slot].data = (uint64_t)mem.device_pointer;
}

void OneapiDevice::tex_free(device_texture &mem)
{
  if (mem.device_pointer) {
    generic_free(mem);
  }
}

unique_ptr<DeviceQueue> OneapiDevice::gpu_queue_create()
{
  return make_unique<OneapiDeviceQueue>(this);
}

bool OneapiDevice::should_use_graphics_interop()
{
  // NOTE(sirgienko) oneAPI doesn't yet support direct writing into graphics API objects, so return
  // false
  return false;
}

void *OneapiDevice::usm_aligned_alloc_host(size_t memory_size, size_t alignment)
{
  assert(device_queue);
  return (oneapi_dll.oneapi_usm_aligned_alloc_host)(device_queue, memory_size, alignment);
}

void OneapiDevice::usm_free(void *usm_ptr)
{
  assert(device_queue);
  return (oneapi_dll.oneapi_usm_free)(device_queue, usm_ptr);
}

CCL_NAMESPACE_END

#endif
