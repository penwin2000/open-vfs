#include "input.hpp"

#include <algorithm>
#include <cmath>
#include <open_vfs/macros.hpp>

namespace open_vfs::input {

OPEN_VFS_IMPL
Vec2f apply_radial_deadzone(Vec2f raw_stick, float deadzone) noexcept {
    const float clamped_dz = std::clamp(deadzone, 0.0f, 0.99f);
    const float magnitude = std::sqrt((raw_stick.x * raw_stick.x) + (raw_stick.y * raw_stick.y));
    if (magnitude <= clamped_dz || magnitude == 0.0f) {
        return Vec2f{0.0f, 0.0f};
    }
    const float clamped_mag = std::min(magnitude, 1.0f);
    const float normalized_scale = (clamped_mag - clamped_dz) / (1.0f - clamped_dz);
    const float factor = normalized_scale / magnitude;
    return Vec2f{raw_stick.x * factor, raw_stick.y * factor};
}

OPEN_VFS_IMPL
InputAction map_key_to_action(uint32_t virtual_key_code) noexcept {
    switch (virtual_key_code) {
        case 'W': return InputAction::MoveForward;
        case 'S': return InputAction::MoveBackward;
        case 'A': return InputAction::MoveLeft;
        case 'D': return InputAction::MoveRight;
        case 0x20: return InputAction::Jump;      // VK_SPACE
        case 'E': return InputAction::Interact;
        case 0x10: return InputAction::Sprint;    // VK_SHIFT
        case 0x1B: return InputAction::Pause;     // VK_ESCAPE
        default: return InputAction::None;
    }
}

OPEN_VFS_IMPL
InputState merge_input_states(const InputState& kb_mouse, const InputState& gamepad) noexcept {
    InputState merged;
    merged.move_axis.x = std::clamp(kb_mouse.move_axis.x + gamepad.move_axis.x, -1.0f, 1.0f);
    merged.move_axis.y = std::clamp(kb_mouse.move_axis.y + gamepad.move_axis.y, -1.0f, 1.0f);
    merged.look_delta.x = kb_mouse.look_delta.x + gamepad.look_delta.x;
    merged.look_delta.y = kb_mouse.look_delta.y + gamepad.look_delta.y;
    merged.action_mask = kb_mouse.action_mask | gamepad.action_mask;
    merged.left_trigger = std::max(kb_mouse.left_trigger, gamepad.left_trigger);
    merged.right_trigger = std::max(kb_mouse.right_trigger, gamepad.right_trigger);
    return merged;
}

OPEN_VFS_STUB
Status poll_xinput_gamepad(uint32_t slot_index, InputState& out_state) {
    out_state = InputState{};
    if (slot_index >= 4) {
        return Status::InvalidArgument;
    }
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status poll_raw_mouse(InputState& out_state) {
    out_state = InputState{};
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status poll_keyboard_state(InputState& out_state) {
    out_state = InputState{};
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status load_input_bindings(std::string_view config_text) {
    (void)config_text;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status set_gamepad_rumble(uint32_t slot_index, float left_motor, float right_motor) {
    (void)left_motor;
    (void)right_motor;
    if (slot_index >= 4) {
        return Status::InvalidArgument;
    }
    return Status::NotImplemented;
}

}  // namespace open_vfs::input
