/**
 * @file src/platform/windows/display_amd.cpp
 * @brief AMD Direct Capture backend.
 */

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <d3dcompiler.h>
#include <filesystem>

#include "display.h"
#include "display_vram.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/video.h"

#include <AMF/components/DisplayCapture.h>
#include <AMF/core/Trace.h>

#if !defined(SUNSHINE_SHADERS_DIR)
  #define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/directx"
#endif

namespace platf {
  using namespace std::literals;
}

namespace platf::dxgi {
  namespace {
    blend_t make_blend(device_t::pointer device, bool enable, bool invert) {
      D3D11_BLEND_DESC bdesc {};
      auto &rt = bdesc.RenderTarget[0];
      rt.BlendEnable = enable;
      rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

      if (enable) {
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        if (invert) {
          rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
          rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
        } else {
          rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
          rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        }
        rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
      }

      blend_t blend;
      auto status = device->CreateBlendState(&bdesc, &blend);
      if (status) {
        BOOST_LOG(error) << "AMD DirectCapture: failed to create blend state [0x"sv << util::hex(status).to_string_view() << ']';
        return nullptr;
      }
      return blend;
    }

    blob_t compile_shader_file(const char *file, const char *entrypoint, const char *shader_model) {
      blob_t::pointer compiled_p {};
      blob_t::pointer msg_p {};
      const auto path = std::filesystem::path(SUNSHINE_SHADERS_DIR) / file;
      const auto wfile = path.wstring();
      UINT flags = 0;
#ifdef _DEBUG
      flags |= D3DCOMPILE_DEBUG;
#endif
      const auto status = D3DCompileFromFile(wfile.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entrypoint, shader_model, flags, 0, &compiled_p, &msg_p);
      if (FAILED(status)) {
        BOOST_LOG(error) << "AMD DirectCapture: couldn't compile shader [" << path.string() << "] [0x" << util::hex(status).to_string_view() << ']';
        if (msg_p) {
          BOOST_LOG(error) << "AMD DirectCapture shader error: " << (const char *) msg_p->GetBufferPointer();
        }
        return nullptr;
      }
      return blob_t {compiled_p};
    }
  }  // namespace

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

    // Keep same behavior as foundation baseline: present-synced + duplicate output.
    capture_comp->SetProperty(AMF_DISPLAYCAPTURE_MONITOR_INDEX, output_index);
    capture_comp->SetProperty(AMF_DISPLAYCAPTURE_FRAMERATE, AMFConstructRate(config.framerate, 1));
    capture_comp->SetProperty(AMF_DISPLAYCAPTURE_MODE, AMF_DISPLAYCAPTURE_MODE_GET_CURRENT_SURFACE);
    capture_comp->SetProperty(AMF_DISPLAYCAPTURE_DUPLICATEOUTPUT, false);

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

    auto vs_blob = compile_shader_file("simple_cursor_vs.hlsl", "main_vs", "vs_5_0");
    auto ps_blob = compile_shader_file("simple_cursor_ps.hlsl", "main_ps", "ps_5_0");
    if (!vs_blob || !ps_blob) {
      return -1;
    }

    auto status = device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &cursor_vs);
    if (status) {
      BOOST_LOG(error) << "AMD DirectCapture: failed to create cursor VS [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    status = device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &cursor_ps);
    if (status) {
      BOOST_LOG(error) << "AMD DirectCapture: failed to create cursor PS [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    blend_invert = make_blend(device.get(), true, true);
    blend_disable = make_blend(device.get(), false, false);
    if (!blend_invert || !blend_disable) {
      return -1;
    }

    D3D11_BUFFER_DESC buffer_desc {};
    buffer_desc.ByteWidth = sizeof(float[4]);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buf_t::pointer cursor_info_p {};
    status = device->CreateBuffer(&buffer_desc, nullptr, &cursor_info_p);
    if (status) {
      BOOST_LOG(error) << "AMD DirectCapture: failed to create cursor constant buffer [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    cursor_info = buf_t {cursor_info_p};

    return 0;
  }

  capture_e display_amd_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
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

    if (cursor_visible) {
      CURSORINFO ci {};
      ci.cbSize = sizeof(CURSORINFO);
      if (GetCursorInfo(&ci) && ci.flags == CURSOR_SHOWING) {
        const float cursor_data[4] = {
          static_cast<float>(ci.ptScreenPos.x),
          static_cast<float>(ci.ptScreenPos.y),
          static_cast<float>(width),
          static_cast<float>(height)
        };
        device_ctx->UpdateSubresource(cursor_info.get(), 0, nullptr, cursor_data, 0, 0);
        device_ctx->VSSetConstantBuffers(0, 1, &cursor_info);
        device_ctx->VSSetShader(cursor_vs.get(), nullptr, 0);
        device_ctx->PSSetShader(cursor_ps.get(), nullptr, 0);
        device_ctx->OMSetRenderTargets(1, &d3d_img->capture_rt, nullptr);
        device_ctx->IASetInputLayout(nullptr);
        device_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        device_ctx->OMSetBlendState(blend_invert.get(), nullptr, 0x00FFFFFFu);
        device_ctx->Draw(3, 0);
        ID3D11RenderTargetView *empty_rt = nullptr;
        device_ctx->OMSetRenderTargets(1, &empty_rt, nullptr);
        device_ctx->OMSetBlendState(blend_disable.get(), nullptr, 0x00FFFFFFu);
      }
    }

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
