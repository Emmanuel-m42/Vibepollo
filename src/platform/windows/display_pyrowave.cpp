/**
 * @file src/platform/windows/display_pyrowave.cpp
 * @brief PyroWave encode device: shares captured D3D11 frames with the PyroWave Vulkan encoder.
 *
 * Each captured frame (an NT-handle shared D3D11 texture guarded by a keyed mutex, see
 * display_vram_t::complete_img) is GPU-copied into a staging texture owned by this device, so the
 * capture side is never blocked by encoding. The staging texture is imported into Vulkan once, and
 * a shared D3D11 fence imported as a Vulkan timeline semaphore orders the copy before the encode,
 * as in PyroWave's own desktop capture sample. PyroWave scales the image to the client resolution
 * and converts RGB to YCbCr itself.
 */
// standard includes
#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <vector>

// platform includes
#include <winsock2.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

// local includes
#include "display.h"
#include "display_vram.h"
#include "pyrowave_loader.h"
#include "src/logging.h"
#include "src/utility.h"
#include "src/video.h"

namespace platf::dxgi {
  using namespace std::literals;

  using device5_t = util::safe_ptr<ID3D11Device5, Release<ID3D11Device5>>;
  using device_ctx4_t = util::safe_ptr<ID3D11DeviceContext4, Release<ID3D11DeviceContext4>>;
  using fence_t = util::safe_ptr<ID3D11Fence, Release<ID3D11Fence>>;

  namespace {
    // Moonlight PyroWave frame container: "PYRW", version, big-endian u16 packet count,
    // flags byte, then per packet a big-endian u32 length followed by the packet bytes.
    constexpr uint8_t pyrw_version = 1;
    // Frames produced by PyroWave's own RGB -> YCbCr conversion: BT.709, full range,
    // centre-sited chroma. Without this flag clients assume limited range, left-cosited chroma.
    constexpr uint8_t pyrw_flag_full_range_center_chroma = 0x01;

    // PyroWave packets are only a framing unit inside our container; Moonlight's RTP layer
    // splits the whole frame into network packets, so large PyroWave packets are fine.
    constexpr size_t pyrowave_packet_boundary = 16 * 1024;

    constexpr UINT keyed_mutex_timeout_ms = 3000;

    struct vk_format_t {
      VkFormat format;
      VkColorSpaceKHR color_space;
    };

    std::optional<vk_format_t> vk_format_from_dxgi(DXGI_FORMAT format) {
      switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
          return vk_format_t {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        case DXGI_FORMAT_R8G8B8A8_UNORM:
          return vk_format_t {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
          // scRGB capture (HDR / WCG desktop); PyroWave converts it down to SDR.
          return vk_format_t {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT};
        default:
          return std::nullopt;
      }
    }

    void append_be32(std::vector<uint8_t> &out, uint32_t value) {
      out.push_back(uint8_t(value >> 24));
      out.push_back(uint8_t(value >> 16));
      out.push_back(uint8_t(value >> 8));
      out.push_back(uint8_t(value));
    }
  }  // namespace

  class d3d_pyrowave_encode_device_t final: public pyrowave_encode_device_t {
  public:
    ~d3d_pyrowave_encode_device_t() override {
      if (!pw) {
        return;
      }
      if (encoder) {
        pw->encoder_destroy(encoder);
      }
      destroy_shared(staging_image);
      destroy_shared(black_image);
      if (sync) {
        pw->sync_object_destroy(sync);
      }
      if (pw_device) {
        pw->device_destroy(pw_device);
      }
    }

    bool init_device(const std::shared_ptr<display_vram_t> &display, IDXGIAdapter1 *adapter) {
      pw = pyrowave::api();
      if (!pw) {
        return false;
      }

      D3D_FEATURE_LEVEL feature_levels[] {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
      device_t base_device;
      device_ctx_t base_ctx;
      auto status = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        feature_levels, static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION,
        &base_device, nullptr, &base_ctx
      );
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to create D3D11 device [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }
      if (FAILED(base_device->QueryInterface(__uuidof(ID3D11Device5), (void **) &device)) ||
          FAILED(base_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **) &device_ctx))) {
        BOOST_LOG(error) << "PyroWave: D3D11 fences are unavailable (needs Windows 10 1703 or newer)"sv;
        return false;
      }

      status = device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **) &fence);
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to create shared fence [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }
      HANDLE fence_handle = nullptr;
      status = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle);
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to share fence [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }

      DXGI_ADAPTER_DESC adapter_desc;
      adapter->GetDesc(&adapter_desc);
      pyrowave_luid luid {};
      static_assert(sizeof(luid.luid) == sizeof(adapter_desc.AdapterLuid));
      std::memcpy(luid.luid, &adapter_desc.AdapterLuid, sizeof(luid.luid));
      // HIGH priority puts encoding on an async compute queue so it is not stuck behind the game.
      auto result = pw->create_device_by_compat2(0, 0, nullptr, nullptr, &luid, VK_QUEUE_GLOBAL_PRIORITY_HIGH, &pw_device);
      if (result != PYROWAVE_SUCCESS) {
        BOOST_LOG(error) << "PyroWave: no Vulkan device for this adapter (error "sv << static_cast<int>(result) << ')';
        CloseHandle(fence_handle);
        return false;
      }

      // PyroWave takes ownership of the handle on successful import.
      pyrowave_sync_object_create_info sync_info {};
      sync_info.device = pw_device;
      sync_info.external_handle = reinterpret_cast<pyrowave_os_handle>(fence_handle);
      sync_info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
      sync_info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
      result = pw->sync_object_create(&sync_info, &sync);
      if (result != PYROWAVE_SUCCESS) {
        BOOST_LOG(error) << "PyroWave: failed to import the D3D11 fence (error "sv << static_cast<int>(result) << ')';
        CloseHandle(fence_handle);
        return false;
      }

      display_width = display->width;
      display_height = display->height;
      return create_black_image();
    }

    bool init_encoder(const ::video::config_t &client_config) override {
      // 4:2:0 needs even dimensions.
      width = client_config.width & ~1;
      height = client_config.height & ~1;
      framerate = std::max(1, client_config.framerate);
      chroma444 = client_config.chromaSamplingType == 1;

      pyrowave_encoder_create_info create_info {};
      create_info.device = pw_device;
      create_info.width = width;
      create_info.height = height;
      create_info.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
      const auto result = pw->encoder_create(&create_info, &encoder);
      if (result != PYROWAVE_SUCCESS) {
        BOOST_LOG(error) << "PyroWave: encoder creation failed for "sv << width << 'x' << height << " (error "sv << static_cast<int>(result) << ')';
        return false;
      }

      set_bitrate(client_config.bitrate);
      BOOST_LOG(info) << "PyroWave: encoding "sv << width << 'x' << height << '@' << framerate
                      << (chroma444 ? " 4:4:4"sv : " 4:2:0"sv) << ", "sv << client_config.bitrate << " kbps ("sv
                      << max_frame_bytes << " bytes per frame)"sv;
      return true;
    }

    void set_bitrate(int bitrate_kbps) override {
      // PyroWave's rate control is an exact per-frame byte budget.
      const size_t bytes = static_cast<size_t>(std::max(bitrate_kbps, 1000)) * 1000 / 8 / static_cast<size_t>(framerate);
      max_frame_bytes = std::max<size_t>(bytes, 16 * 1024);
    }

    int convert(platf::img_t &img_base) override {
      if (!encoder) {
        return -1;
      }
      staged = nullptr;
      prune_expired_captures();

      auto &img = static_cast<img_d3d_t &>(img_base);
      if (img.blank || !img.encoder_texture_handle) {
        staged = &black_image;
      } else {
        auto *capture = open_capture(img);
        if (!capture) {
          return -1;
        }

        D3D11_TEXTURE2D_DESC desc;
        capture->texture->GetDesc(&desc);
        if (!ensure_staging(desc)) {
          return -1;
        }

        const auto status = capture->mutex->AcquireSync(0, keyed_mutex_timeout_ms);
        if (status != S_OK) {
          BOOST_LOG(error) << "PyroWave: failed to acquire the capture texture [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }
        device_ctx->CopyResource(staging_image.texture.get(), capture->texture.get());
        capture->mutex->ReleaseSync(0);
        staged = &staging_image;
      }

      // Everything submitted on our device so far (the copy) happens before Vulkan reads the frame.
      device_ctx->Signal(fence.get(), ++timeline);
      device_ctx->Flush();
      staged_value = timeline;
      return 0;
    }

    bool encode_frame(std::vector<uint8_t> &frame) override {
      if (!staged) {
        return false;
      }

      pyrowave_gpu_external_reference external_ref {};
      external_ref.image = staged->image;
      external_ref.queue_family_index = VK_QUEUE_FAMILY_EXTERNAL;

      pyrowave_gpu_sync_operation acquire {};
      acquire.sync.semaphore = pw->sync_object_get_semaphore(sync);
      acquire.sync.value = staged_value;
      acquire.images = &external_ref;
      acquire.num_images = 1;

      pyrowave_gpu_sync_operation release {};
      release.sync.semaphore = pw->sync_object_get_semaphore(sync);
      release.sync.value = ++timeline;

      pyrowave_scaled_encode_info scaled_info {};
      auto result = pw->image_get_image_view(staged->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &scaled_info.view);
      if (result != PYROWAVE_SUCCESS) {
        BOOST_LOG(error) << "PyroWave: failed to create an image view (error "sv << static_cast<int>(result) << ')';
        staged = nullptr;
        return false;
      }
      scaled_info.input_color_space = staged->color_space;
      scaled_info.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      scaled_info.ycbcr_chroma_midpoint = 128.0f / 255.0f;
      scaled_info.intermediate_plane_format = VK_FORMAT_R8_UNORM;

      pyrowave_rate_control rate_control {max_frame_bytes};
      result = pw->encoder_encode_gpu_scaled_synchronous(encoder, &acquire, &release, &scaled_info, &rate_control);
      staged = nullptr;

      if (result != PYROWAVE_SUCCESS) {
        // Nothing was submitted, so Vulkan will never signal the release value. Reach it on our
        // own timeline so the fence stays monotonic and later frames do not wait on it forever.
        device_ctx->Signal(fence.get(), timeline);
        device_ctx->Flush();
        BOOST_LOG(error) << "PyroWave: encode failed (error "sv << static_cast<int>(result) << ')';
        return false;
      }

      // The release value is signalled once Vulkan is done with the image; the next copy into
      // the staging texture must wait for it.
      device_ctx->Wait(fence.get(), timeline);

      // Blocks until the GPU encode has finished.
      size_t num_packets = 0;
      result = pw->encoder_compute_num_packets(encoder, pyrowave_packet_boundary, &num_packets);
      if (result != PYROWAVE_SUCCESS || num_packets == 0 || num_packets > 0xffff) {
        BOOST_LOG(error) << "PyroWave: packet count query failed (error "sv << static_cast<int>(result) << ", "sv << num_packets << " packets)"sv;
        return false;
      }
      packets.resize(num_packets);
      bitstream.resize(max_frame_bytes + num_packets * 64 + 4096);
      size_t out_packets = 0;
      result = pw->encoder_packetize(encoder, packets.data(), pyrowave_packet_boundary, &out_packets, bitstream.data(), bitstream.size());
      if (result != PYROWAVE_SUCCESS || out_packets == 0) {
        BOOST_LOG(error) << "PyroWave: packetize failed (error "sv << static_cast<int>(result) << ')';
        return false;
      }

      size_t payload_bytes = 0;
      for (size_t i = 0; i < out_packets; i++) {
        payload_bytes += packets[i].size;
      }
      frame.clear();
      frame.reserve(8 + out_packets * 4 + payload_bytes);
      frame.insert(frame.end(), {'P', 'Y', 'R', 'W', pyrw_version, uint8_t(out_packets >> 8), uint8_t(out_packets & 0xff), pyrw_flag_full_range_center_chroma});
      for (size_t i = 0; i < out_packets; i++) {
        append_be32(frame, static_cast<uint32_t>(packets[i].size));
        const auto *begin = bitstream.data() + packets[i].offset;
        frame.insert(frame.end(), begin, begin + packets[i].size);
      }
      return true;
    }

  private:
    /// A texture created on our device and imported into Vulkan.
    struct shared_image_t {
      texture2d_t texture;
      pyrowave_image image = nullptr;
      VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      D3D11_TEXTURE2D_DESC desc {};
    };

    /// A capture texture opened on our device.
    struct capture_texture_t {
      texture2d_t texture;
      keyed_mutex_t mutex;
      const ID3D11Texture2D *capture_texture_p = nullptr;
      std::weak_ptr<const platf::img_t> img_weak;
    };

    void destroy_shared(shared_image_t &shared) {
      if (shared.image) {
        pw->image_destroy(shared.image);
        shared.image = nullptr;
      }
      shared.texture.reset();
    }

    void prune_expired_captures() {
      for (auto it = captures.begin(); it != captures.end();) {
        if (it->second.img_weak.expired()) {
          it = captures.erase(it);
        } else {
          ++it;
        }
      }
    }

    bool import_to_vulkan(shared_image_t &shared, HANDLE shared_handle, UINT tex_width, UINT tex_height, DXGI_FORMAT dxgi_format) {
      const auto format = vk_format_from_dxgi(dxgi_format);
      if (!format) {
        BOOST_LOG(error) << "PyroWave: unsupported capture format "sv << static_cast<int>(dxgi_format);
        CloseHandle(shared_handle);
        return false;
      }

      VkImageCreateInfo image_create_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image_create_info.imageType = VK_IMAGE_TYPE_2D;
      image_create_info.extent = {tex_width, tex_height, 1};
      image_create_info.format = format->format;
      image_create_info.mipLevels = 1;
      image_create_info.arrayLayers = 1;
      image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
      image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
      image_create_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

      // PyroWave takes ownership of the handle on successful import.
      pyrowave_image_create_info image_info {};
      create_info.device = pw_device;
      image_info.external_handle = reinterpret_cast<pyrowave_os_handle>(shared_handle);
      image_info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
      image_info.image_create_info = &image_create_info;
      const auto result = pw->image_create(&image_info, &shared.image);
      if (result != PYROWAVE_SUCCESS) {
        BOOST_LOG(error) << "PyroWave: failed to import a "sv << tex_width << 'x' << tex_height << " D3D11 texture into Vulkan (error "sv << static_cast<int>(result) << ')';
        CloseHandle(shared_handle);
        return false;
      }
      shared.color_space = format->color_space;
      return true;
    }

    capture_texture_t *open_capture(const img_d3d_t &img) {
      auto &capture = captures[img.id];
      if (capture.texture && capture.capture_texture_p == img.capture_texture.get()) {
        return &capture;
      }
      // Textures change when a dummy image becomes a real one.
      capture = {};

      device1_t device1;
      if (FAILED(device->QueryInterface(__uuidof(ID3D11Device1), (void **) &device1))) {
        return nullptr;
      }
      auto status = device1->OpenSharedResource1(img.encoder_texture_handle, __uuidof(ID3D11Texture2D), (void **) &capture.texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to open the shared capture texture [0x"sv << util::hex(status).to_string_view() << ']';
        captures.erase(img.id);
        return nullptr;
      }
      status = capture.texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &capture.mutex);
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: capture texture has no keyed mutex [0x"sv << util::hex(status).to_string_view() << ']';
        captures.erase(img.id);
        return nullptr;
      }
      capture.capture_texture_p = img.capture_texture.get();
      capture.img_weak = img.weak_from_this();
      return &capture;
    }

    /// Creates a texture on our device, shares it and imports it into Vulkan.
    bool create_shared(shared_image_t &shared, UINT tex_width, UINT tex_height, DXGI_FORMAT format) {
      destroy_shared(shared);

      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = tex_width;
      desc.Height = tex_height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = format;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

      auto status = device->CreateTexture2D(&desc, nullptr, &shared.texture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to create a "sv << tex_width << 'x' << tex_height << " staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }

      resource1_t resource;
      status = shared.texture->QueryInterface(__uuidof(IDXGIResource1), (void **) &resource);
      if (FAILED(status)) {
        destroy_shared(shared);
        return false;
      }
      HANDLE shared_handle = nullptr;
      status = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared_handle);
      if (FAILED(status)) {
        BOOST_LOG(error) << "PyroWave: failed to share the staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        destroy_shared(shared);
        return false;
      }
      if (!import_to_vulkan(shared, shared_handle, tex_width, tex_height, format)) {
        destroy_shared(shared);
        return false;
      }
      shared.desc = desc;
      return true;
    }

    bool ensure_staging(const D3D11_TEXTURE2D_DESC &capture_desc) {
      if (staging_image.image && staging_image.desc.Width == capture_desc.Width &&
          staging_image.desc.Height == capture_desc.Height && staging_image.desc.Format == capture_desc.Format) {
        return true;
      }
      BOOST_LOG(info) << "PyroWave: capture is "sv << capture_desc.Width << 'x' << capture_desc.Height << ", format "sv << static_cast<int>(capture_desc.Format);
      return create_shared(staging_image, capture_desc.Width, capture_desc.Height, capture_desc.Format);
    }

    bool create_black_image() {
      if (!create_shared(black_image, std::max(display_width, 16), std::max(display_height, 16), DXGI_FORMAT_B8G8R8A8_UNORM)) {
        return false;
      }
      render_target_t rtv;
      if (FAILED(device->CreateRenderTargetView(black_image.texture.get(), nullptr, &rtv))) {
        return false;
      }
      const float black[] {0.0f, 0.0f, 0.0f, 1.0f};
      device_ctx->ClearRenderTargetView(rtv.get(), black);
      device_ctx->Flush();
      return true;
    }

    const pyrowave::api_t *pw = nullptr;
    pyrowave_device pw_device = nullptr;
    pyrowave_encoder encoder = nullptr;
    pyrowave_sync_object sync = nullptr;

    device5_t device;
    device_ctx4_t device_ctx;
    fence_t fence;
    uint64_t timeline = 0;

    std::map<uint32_t, capture_texture_t> captures;
    shared_image_t staging_image;
    shared_image_t black_image;

    shared_image_t *staged = nullptr;
    uint64_t staged_value = 0;

    std::vector<pyrowave_packet> packets;
    std::vector<uint8_t> bitstream;

    int display_width = 0;
    int display_height = 0;
    int width = 0;
    int height = 0;
    int framerate = 60;
    bool chroma444 = false;
    size_t max_frame_bytes = 0;
  };

  std::unique_ptr<pyrowave_encode_device_t> display_vram_t::make_pyrowave_encode_device() {
    auto device = std::make_unique<d3d_pyrowave_encode_device_t>();
    if (!device->init_device(shared_from_this(), adapter.get())) {
      return nullptr;
    }
    return device;
  }

}  // namespace platf::dxgi
