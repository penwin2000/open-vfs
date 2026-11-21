#pragma once

#include <cstdint>
#include <string_view>

#include <open_vfs/status.hpp>

namespace open_vfs::input {

enum class InputAction : uint32_t {
    None = 0,
    MoveForward = 1,
    MoveBackward = 2,
    MoveLeft = 3,
    MoveRight = 4,
    Jump = 5,
    Interact = 6,
    Sprint = 7,
    Pause = 8,
};

struct Vec2f {
    float x = 0.0f;
    float y = 0.0f;
};

struct InputState {
    Vec2f move_axis;
    Vec2f look_delta;
    uint32_t action_mask = 0;
    float left_trigger = 0.0f;
    float right_trigger = 0.0f;
};

Vec2f apply_radial_deadzone(Vec2f raw_stick, float deadzone) noexcept;

InputAction map_key_to_action(uint32_t virtual_key_code) noexcept;

InputState merge_input_states(const InputState& kb_mouse, const InputState& gamepad) noexcept;

Status poll_xinput_gamepad(uint32_t slot_index, InputState& out_state);

Status poll_raw_mouse(InputState& out_state);

Status poll_keyboard_state(InputState& out_state);

Status load_input_bindings(std::string_view config_text);

Status set_gamepad_rumble(uint32_t slot_index, float left_motor, float right_motor);

}  // namespace open_vfs::input
