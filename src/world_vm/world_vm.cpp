#include "world_vm.hpp"

#include <algorithm>
#include <open_vfs/macros.hpp>

namespace open_vfs::world_vm {

OPEN_VFS_IMPL
EntityId create_entity(EntityRegistry& registry) {
    if (!registry.free_list.empty()) {
        const uint32_t idx = registry.free_list.back();
        registry.free_list.pop_back();
        auto& slot = registry.slots[idx];
        slot.alive = true;
        return EntityId{idx, slot.generation};
    }

    const uint32_t idx = static_cast<uint32_t>(registry.slots.size());
    EntitySlot slot;
    slot.generation = 1;
    slot.alive = true;
    registry.slots.push_back(slot);
    return EntityId{idx, slot.generation};
}

OPEN_VFS_IMPL
bool destroy_entity(EntityRegistry& registry, EntityId entity) noexcept {
    if (!is_entity_alive(registry, entity)) {
        return false;
    }
    auto& slot = registry.slots[entity.index];
    slot.alive = false;
    ++slot.generation;
    registry.free_list.push_back(entity.index);
    return true;
}

OPEN_VFS_IMPL
bool is_entity_alive(const EntityRegistry& registry, EntityId entity) noexcept {
    if (entity.index >= registry.slots.size() || entity.generation == 0) {
        return false;
    }
    const auto& slot = registry.slots[entity.index];
    return slot.alive && slot.generation == entity.generation;
}

OPEN_VFS_IMPL
uint32_t step_fixed_timestep(SimulationClock& clock,
                             double delta_seconds,
                             uint32_t max_substeps) noexcept {
    if (clock.fixed_dt <= 0.0 || delta_seconds <= 0.0 || max_substeps == 0) {
        return 0;
    }
    const double clamped_delta = std::min(delta_seconds, clock.fixed_dt * static_cast<double>(max_substeps));
    clock.accumulator += clamped_delta;

    uint32_t steps = 0;
    while (clock.accumulator >= clock.fixed_dt && steps < max_substeps) {
        clock.accumulator -= clock.fixed_dt;
        ++clock.total_ticks;
        ++steps;
    }
    return steps;
}

OPEN_VFS_STUB
Status load_world_manifest(std::span<const uint8_t> manifest_bytes,
                           WorldManifest& out_manifest) {
    (void)manifest_bytes;
    out_manifest = WorldManifest{};
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status stream_world_sector(const WorldManifest& manifest,
                           Vec3f focus_position,
                           EntityRegistry& registry) {
    (void)manifest;
    (void)focus_position;
    (void)registry;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status step_rigid_body_physics(EntityRegistry& registry, double fixed_dt) {
    (void)registry;
    (void)fixed_dt;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status load_script_bytecode(std::span<const uint8_t> script_bytes,
                            ScriptProgram& out_program) {
    (void)script_bytes;
    out_program = ScriptProgram{};
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status execute_vm_tick(const ScriptProgram& program,
                       ScriptVmState& vm_state,
                       uint32_t instruction_budget) {
    (void)program;
    (void)vm_state;
    (void)instruction_budget;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status dispatch_native_call(uint64_t native_hash,
                            std::span<uint64_t> args,
                            uint64_t& out_return_value) {
    (void)native_hash;
    (void)args;
    out_return_value = 0;
    return Status::NotImplemented;
}

}  // namespace open_vfs::world_vm
