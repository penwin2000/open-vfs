#include "test_harness.hpp"
#include "input/input.hpp"

int main() {
    using namespace open_vfs;
    using namespace open_vfs::input;

    // 1. Radial thumbstick deadzone filtering
    const auto zeroed = apply_radial_deadzone(Vec2f{0.10f, 0.05f}, 0.20f);
    OVF_EXPECT_NEAR(zeroed.x, 0.0f, 1e-5f);
    OVF_EXPECT_NEAR(zeroed.y, 0.0f, 1e-5f);

    const auto rescaled = apply_radial_deadzone(Vec2f{0.60f, 0.0f}, 0.20f);
    OVF_EXPECT_NEAR(rescaled.x, 0.50f, 1e-5f);
    OVF_EXPECT_NEAR(rescaled.y, 0.0f, 1e-5f);

    // 2. Virtual key to logical action mapping
    OVF_EXPECT(map_key_to_action('W') == InputAction::MoveForward);
    OVF_EXPECT(map_key_to_action(0x20) == InputAction::Jump);
    OVF_EXPECT(map_key_to_action(0x1B) == InputAction::Pause);
    OVF_EXPECT(map_key_to_action('Z') == InputAction::None);

    // 3. State merging across keyboard/mouse and gamepad
    InputState kb{};
    kb.move_axis = Vec2f{0.8f, 0.0f};
    kb.action_mask = 0x01u;
    kb.right_trigger = 0.2f;

    InputState pad{};
    pad.move_axis = Vec2f{0.5f, -0.4f};
    pad.action_mask = 0x04u;
    pad.right_trigger = 0.9f;

    const auto merged = merge_input_states(kb, pad);
    OVF_EXPECT_NEAR(merged.move_axis.x, 1.0f, 1e-5f);
    OVF_EXPECT_NEAR(merged.move_axis.y, -0.4f, 1e-5f);
    OVF_EXPECT(merged.action_mask == 0x05u);
    OVF_EXPECT_NEAR(merged.right_trigger, 0.9f, 1e-5f);

    // 4. Pending polling/rumble stubs
    InputState polled{};
    OVF_EXPECT(poll_xinput_gamepad(0, polled) == Status::NotImplemented);
    OVF_EXPECT(poll_xinput_gamepad(4, polled) == Status::InvalidArgument);
    OVF_EXPECT(set_gamepad_rumble(0, 0.5f, 0.5f) == Status::NotImplemented);

    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
