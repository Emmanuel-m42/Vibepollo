/**
 * @file src/platform/windows/display_amd.cpp
 * @brief AMD Direct Capture backend.
 */

#include "display.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/video.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
}

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
    if (amfrt_lib) {
      FreeLibrary(amfrt_lib);
      amfrt_lib = nullptr;
    }
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
    amfrt_lib = LoadLibraryW(AMF_DLL_NAME);
    if (!amfrt_lib) {
      auto err = GetLastError();
      BOOST_LOG(error) << "AMD DirectCapture: failed to load AMF runtime DLL '" << AMF_DLL_NAMEA
                       << "', GetLastError=0x" << util::hex(err).to_string_view();
      return -1;
    }

    auto fn_query_version = (AMFQueryVersion_Fn) GetProcAddress(amfrt_lib, AMF_QUERY_VERSION_FUNCTION_NAME);
    auto fn_init = (AMFInit_Fn) GetProcAddress(amfrt_lib, AMF_INIT_FUNCTION_NAME);
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

    // Foundation behavior: present-synced capture with DuplicateOutput path.
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
    BOOST_LOG(info) << "AMD DirectCapture: init succeeded (monitor_index=" << output_index
                    << ", requested_fps=" << config.framerate << ")";
    return 0;
  }

}  // namespace platf::dxgi
