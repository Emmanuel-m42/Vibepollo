/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <vector>

// local includes
#include "crypto.h"
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;
  struct amd_gaze_foveation_state_t {
    bool enabled = false;
    bool valid_tracking = false;
    std::uint32_t timestamp_ms = 0;
    float center_u = 0.5f;
    float center_v = 0.5f;
    float inner_radius = 0.12f;
    float outer_radius = 0.28f;
    std::uint8_t strength = 50;
    std::uint8_t profile = 0;
  };

  void print(void *input);
  void reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data, const crypto::PERM &permission);

#ifdef SUNSHINE_TESTS
  bool validate_packet_for_tests(const std::vector<std::uint8_t> &input_data);
#endif

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  std::shared_ptr<input_t> alloc(safe::mail_t mail);
  amd_gaze_foveation_state_t get_amd_gaze_foveation_state();

  struct touch_port_t: public platf::touch_port_t {
    int env_width, env_height;

    // Offset x and y coordinates of the client
    float client_offsetX, client_offsetY;

    float scalar_inv;

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
