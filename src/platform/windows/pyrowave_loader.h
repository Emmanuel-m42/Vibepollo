/**
 * @file src/platform/windows/pyrowave_loader.h
 * @brief Runtime loading of the PyroWave encoder library (libpyrowave-shared-0.dll).
 *
 * PyroWave is loaded at runtime rather than linked so that a missing DLL or a GPU
 * without a suitable Vulkan 1.3 driver only disables PyroWave instead of Sunshine.
 */
#pragma once

// clang-format off
#include <vulkan/vulkan.h>
#include <pyrowave/pyrowave.h>
// clang-format on

namespace platf::pyrowave {

  struct api_t {
    decltype(&::pyrowave_get_api_version) get_api_version;
    decltype(&::pyrowave_create_default_device) create_default_device;
    decltype(&::pyrowave_create_device_by_compat2) create_device_by_compat2;
    decltype(&::pyrowave_device_destroy) device_destroy;
    decltype(&::pyrowave_device_report_performance_stats) device_report_performance_stats;
    decltype(&::pyrowave_sync_object_create) sync_object_create;
    decltype(&::pyrowave_sync_object_get_semaphore) sync_object_get_semaphore;
    decltype(&::pyrowave_sync_object_destroy) sync_object_destroy;
    decltype(&::pyrowave_image_create) image_create;
    decltype(&::pyrowave_image_get_image_view) image_get_image_view;
    decltype(&::pyrowave_image_destroy) image_destroy;
    decltype(&::pyrowave_encoder_create) encoder_create;
    decltype(&::pyrowave_encoder_encode_gpu_scaled_synchronous) encoder_encode_gpu_scaled_synchronous;
    decltype(&::pyrowave_encoder_compute_num_packets) encoder_compute_num_packets;
    decltype(&::pyrowave_encoder_packetize) encoder_packetize;
    decltype(&::pyrowave_encoder_destroy) encoder_destroy;
  };

  /**
   * @brief Load libpyrowave-shared-0.dll from the executable's directory (once).
   * @return The resolved entry points, or nullptr if the library is unavailable or incompatible.
   */
  const api_t *api();

  /**
   * @brief Whether PyroWave can encode on this machine: the library loads and a
   * Vulkan device satisfying PyroWave's requirements can be created. Cached after the first call.
   */
  bool available();

}  // namespace platf::pyrowave
