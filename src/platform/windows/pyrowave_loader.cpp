/**
 * @file src/platform/windows/pyrowave_loader.cpp
 * @brief Runtime loading of the PyroWave encoder library.
 */
// platform includes
#include <winsock2.h>
#include <windows.h>

// standard includes
#include <filesystem>
#include <mutex>
#include <string>

// local includes
#include "pyrowave_loader.h"
#include "src/logging.h"

namespace platf::pyrowave {
  namespace {
    constexpr wchar_t library_name[] = L"libpyrowave-shared-0.dll";

    template<class T>
    bool resolve(HMODULE module, const char *name, T &fn) {
      fn = reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(module, name)));
      if (!fn) {
        BOOST_LOG(warning) << "PyroWave: missing entry point " << name;
        return false;
      }
      return true;
    }

    const api_t *load() {
      std::wstring exe_path(MAX_PATH, L'\0');
      const auto length = GetModuleFileNameW(nullptr, exe_path.data(), static_cast<DWORD>(exe_path.size()));
      if (length == 0 || length >= exe_path.size()) {
        BOOST_LOG(warning) << "PyroWave: could not determine the executable directory";
        return nullptr;
      }
      exe_path.resize(length);
      const auto library_path = std::filesystem::path(exe_path).parent_path() / library_name;

      // The DLL is only ever loaded from next to sunshine.exe, never from the search path.
      HMODULE module = LoadLibraryExW(library_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (!module) {
        BOOST_LOG(info) << "PyroWave: " << library_path.string() << " not loaded (error " << GetLastError() << "); PyroWave is unavailable";
        return nullptr;
      }

      static api_t loaded {};
      bool ok = resolve(module, "pyrowave_get_api_version", loaded.get_api_version) &&
                resolve(module, "pyrowave_create_default_device", loaded.create_default_device) &&
                resolve(module, "pyrowave_create_device_by_compat2", loaded.create_device_by_compat2) &&
                resolve(module, "pyrowave_device_destroy", loaded.device_destroy) &&
                resolve(module, "pyrowave_sync_object_create", loaded.sync_object_create) &&
                resolve(module, "pyrowave_sync_object_get_semaphore", loaded.sync_object_get_semaphore) &&
                resolve(module, "pyrowave_sync_object_destroy", loaded.sync_object_destroy) &&
                resolve(module, "pyrowave_image_create", loaded.image_create) &&
                resolve(module, "pyrowave_image_get_image_view", loaded.image_get_image_view) &&
                resolve(module, "pyrowave_image_destroy", loaded.image_destroy) &&
                resolve(module, "pyrowave_encoder_create", loaded.encoder_create) &&
                resolve(module, "pyrowave_encoder_encode_gpu_scaled_synchronous", loaded.encoder_encode_gpu_scaled_synchronous) &&
                resolve(module, "pyrowave_encoder_compute_num_packets", loaded.encoder_compute_num_packets) &&
                resolve(module, "pyrowave_encoder_packetize", loaded.encoder_packetize) &&
                resolve(module, "pyrowave_encoder_destroy", loaded.encoder_destroy);
      if (!ok) {
        FreeLibrary(module);
        return nullptr;
      }

      // The API is not stable before 1.0, so require the exact major/minor we were built against.
      uint32_t major = 0, minor = 0, patch = 0;
      loaded.get_api_version(&major, &minor, &patch);
      if (major != PYROWAVE_API_VERSION_MAJOR || minor != PYROWAVE_API_VERSION_MINOR) {
        BOOST_LOG(warning) << "PyroWave: library API " << major << '.' << minor << '.' << patch
                           << " does not match the expected " << PYROWAVE_API_VERSION_MAJOR << '.' << PYROWAVE_API_VERSION_MINOR;
        FreeLibrary(module);
        return nullptr;
      }

      BOOST_LOG(info) << "PyroWave: loaded library API " << major << '.' << minor << '.' << patch;
      return &loaded;
    }
  }  // namespace

  const api_t *api() {
    static std::once_flag once;
    static const api_t *loaded = nullptr;
    std::call_once(once, []() {
      loaded = load();
    });
    return loaded;
  }

  bool available() {
    static std::once_flag once;
    static bool is_available = false;
    std::call_once(once, []() {
      const auto *pw = api();
      if (!pw) {
        return;
      }
      pyrowave_device device = nullptr;
      const auto result = pw->create_default_device(&device);
      if (result != PYROWAVE_SUCCESS || !device) {
        BOOST_LOG(info) << "PyroWave: no suitable Vulkan device (error " << static_cast<int>(result) << "); PyroWave is unavailable";
        return;
      }
      pw->device_destroy(device);
      is_available = true;
      BOOST_LOG(info) << "PyroWave: encoder available";
    });
    return is_available;
  }

}  // namespace platf::pyrowave
