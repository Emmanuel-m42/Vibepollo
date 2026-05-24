/**
 * @file src/platform/windows/display_amd.cpp
 * @brief AMD Direct Capture backend.
 */

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include "display.h"
#include "display_vram.h"
#include "misc.h"
#include "src/config.h"
#include "src/video.h"

#include <AMF/components/DisplayCapture.h>
#include <AMF/core/Trace.h>

namespace platf {
  using namespace std::literals;
}

namespace platf::dxgi {
  amd_capture_t::amd_capture_t() = default;

  amd_capture_t::~amd_capture_t() {
    if (capture_comp) {
      AMF_RESULT result = capture_comp->Drain();
      if (result == AMF_OK) {
        do {
          result = capture_comp->QueryOutput((amf::AMFData **) &captured_surface);
          Sleep(1);
        } while (result != AMF_EOF);
      }
      capture_comp->Terminate();
    }

    if (context) {
      context->Terminate();
    }

    capture_comp = nullptr;
    context = nullptr;
    captured_surface = nullptr;
  }

  capture_e amd_capture_t::release_frame() {
    captured_surface = nullptr;
    return capture_e::ok;
  }

  capture_e amd_capture_t::next_frame(std::chrono::milliseconds timeout, amf::AMFData **out) {
    release_frame();
    if (!capture_comp) {
      return capture_e::error;
    }

    AMF_RESULT result;
    auto start = std::chrono::steady_clock::now();
    do {
      result = capture_comp->QueryOutput(out);
      if (result == AMF_REPEAT) {
        if (std::chrono::steady_clock::now() - start >= timeout) {
          return capture_e::timeout;
        }
        Sleep(1);
      }
    } while (result == AMF_REPEAT);

    if (result != AMF_OK) {
      if (context) {
        auto *d3d_device = static_cast<ID3D11Device *>(context->GetDX11Device(amf::AMF_DX11_1));
        if (d3d_device) {
          auto removed_reason = d3d_device->GetDeviceRemovedReason();
          if (removed_reason != S_OK) {
            BOOST_LOG(error) << "AMD DirectCapture: D3D11 device lost, reason: 0x"sv << util::hex(removed_reason).to_string_view() << ", requesting reinit"sv;
            return capture_e::reinit;
          }
        }
      }

      BOOST_LOG(warning) << "AMD DirectCapture: QueryOutput failed with result: "sv << result;
      return capture_e::timeout;
    }

    return capture_e::ok;
  }

  int amd_capture_t::init(display_base_t *display, const ::video::config_t &config, int output_index) {
    amfrt_lib.reset(LoadLibraryW(AMF_DLL_NAME));
    if (!amfrt_lib) {
      return -1;
    }

    auto fn_query_version = (AMFQueryVersion_Fn) GetProcAddress((HMODULE) amfrt_lib.get(), AMF_QUERY_VERSION_FUNCTION_NAME);
    auto fn_init = (AMFInit_Fn) GetProcAddress((HMODULE) amfrt_lib.get(), AMF_INIT_FUNCTION_NAME);
    if (!fn_query_version || !fn_init) {
      BOOST_LOG(error) << "AMD DirectCapture: missing required AMF runtime functions";
      return -1;
    }

    auto result = fn_query_version(&amf_version);
    if (result != AMF_OK) {
      BOOST_LOG(error) << "AMD DirectCapture: AMFQueryVersion() failed: "sv << result;
      return -1;
    }

    if (amf_version < AMF_MAKE_FULL_VERSION(1, 4, 30, 0)) {
      BOOST_LOG(warning) << "AMD DirectCapture: unsupported AMF runtime version "
                         << AMF_GET_MAJOR_VERSION(amf_version) << '.'
                         << AMF_GET_MINOR_VERSION(amf_version) << '.'
                         << AMF_GET_SUBMINOR_VERSION(amf_version) << '.'
                         << AMF_GET_BUILD_VERSION(amf_version)
                         << " (need >= 1.4.30.0)";
      return -1;
    }

    result = fn_init(AMF_FULL_VERSION, &amf_factory);
    if (result != AMF_OK || !amf_factory) {
      BOOST_LOG(error) << "AMD DirectCapture: AMFInit() failed: "sv << result;
      return -1;
    }

    DXGI_ADAPTER_DESC1 adapter_desc {};
    display->adapter->GetDesc1(&adapter_desc);
    if (adapter_desc.VendorId != 0x1002) {
      BOOST_LOG(info) << "AMD DirectCapture: non-AMD adapter selected; skipping";
      return -1;
    }

    result = amf_factory->CreateContext(&context);
    if (result != AMF_OK || !context) {
      BOOST_LOG(error) << "AMD DirectCapture: CreateContext() failed: "sv << result;
      return -1;
    }

    result = context->InitDX11(display->device.get());
    if (result != AMF_OK) {
      BOOST_LOG(error) << "AMD DirectCapture: InitDX11() failed: "sv << result;
      return -1;
    }

    result = amf_factory->CreateComponent(context, AMFDisplayCapture, &capture_comp);
    if (result != AMF_OK || !capture_comp) {
      BOOST_LOG(error) << "AMD DirectCapture: CreateComponent(AMFDisplayCapture) failed: "sv << result;
      return -1;
    }

    // Keep same behavior as foundation baseline: present-synced + duplicate output.
    capture_comp->SetProperty(AMF_DISPLAYCAPTURE_MONITOR_INDEX, output_index);
    capture_comp->SetProperty(AMF_DISPLAYCAPTURE_FRAMERATE, AMFConstructRate(config.framerate, 1));
    capture_comp->SetProperty(AMF_DISPLAYCAPTURE_MODE, AMF_DISPLAYCAPTURE_MODE_WAIT_FOR_PRESENT);
    capture_comp->SetProperty(AMF_DISPLAYCAPTURE_DUPLICATEOUTPUT, true);

    result = capture_comp->Init(amf::AMF_SURFACE_UNKNOWN, 0, 0);
    if (result != AMF_OK) {
      BOOST_LOG(error) << "AMD DirectCapture: DisplayCapture::Init() failed: "sv << result;
      return -1;
    }

    capture_comp->GetProperty(AMF_DISPLAYCAPTURE_FORMAT, &capture_format);
    capture_comp->GetProperty(AMF_DISPLAYCAPTURE_RESOLUTION, &resolution);
    display->capture_format = DXGI_FORMAT_UNKNOWN;
    BOOST_LOG(info) << "AMD DirectCapture: resolution [" << resolution.width << 'x' << resolution.height << ']';
    return 0;
  }

  std::optional<int> display_amd_vram_t::monitor_index_from_display_name(const std::string &display_name) {
    if (!adapter || !output) {
      return std::nullopt;
    }

    DXGI_OUTPUT_DESC selected_desc {};
    output->GetDesc(&selected_desc);

    output_t::pointer output_p {};
    for (int idx = 0; adapter->EnumOutputs(idx, &output_p) != DXGI_ERROR_NOT_FOUND; ++idx) {
      output_t out {output_p};
      DXGI_OUTPUT_DESC desc {};
      out->GetDesc(&desc);
      auto name = platf::to_utf8(desc.DeviceName);
      if (name == display_name || wcscmp(desc.DeviceName, selected_desc.DeviceName) == 0) {
        return idx;
      }
    }
    return std::nullopt;
  }

  int display_amd_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name)) {
      return -1;
    }

    auto monitor_index = monitor_index_from_display_name(display_name);
    if (!monitor_index) {
      BOOST_LOG(error) << "AMD DirectCapture: failed to map display to monitor index";
      return -1;
    }

    if (dup.init(this, config, *monitor_index)) {
      return -1;
    }

    return 0;
  }

  capture_e display_amd_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool /*cursor_visible*/) {
    amf::AMFSurfacePtr output_surface;
    auto capture_status = dup.next_frame(timeout, (amf::AMFData **) &output_surface);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }
    dup.captured_surface = output_surface;

    texture2d_t src = (ID3D11Texture2D *) dup.captured_surface->GetPlaneAt(0)->GetNative();
    if (!src) {
      return capture_e::error;
    }

    D3D11_TEXTURE2D_DESC desc {};
    src->GetDesc(&desc);

    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "AMD DirectCapture: size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "AMD DirectCapture format ["sv << dxgi_format_to_string(capture_format) << ']';
    }

    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "AMD DirectCapture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    std::shared_ptr<platf::img_t> img;
    if (!pull_free_image_cb(img)) {
      return capture_e::interrupted;
    }

    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
    d3d_img->blank = false;
    if (complete_img(d3d_img.get(), false) != 0) {
      return capture_e::error;
    }

    bool locked = false;
    if (d3d_img->capture_mutex) {
      auto lock_status = d3d_img->capture_mutex->AcquireSync(0, 3000);
      if (lock_status == S_OK || lock_status == WAIT_ABANDONED) {
        locked = true;
      } else {
        BOOST_LOG(error) << "AMD DirectCapture: failed to acquire texture mutex [0x"sv << util::hex(lock_status).to_string_view() << ']';
      }
    }
    if (!locked) {
      return capture_e::error;
    }
    device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
    d3d_img->capture_mutex->ReleaseSync(0);
    img_out = std::move(img);
    if (img_out) {
      img_out->frame_timestamp = std::chrono::steady_clock::now();
    }
    return capture_e::ok;
  }

  capture_e display_amd_vram_t::release_snapshot() {
    return dup.release_frame();
  }
}  // namespace platf::dxgi
