#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <open_vfs/status.hpp>

namespace open_vfs::world_vm {

struct EntityId {
    uint32_t index = 0;
    uint32_t generation = 0;

    bool operator==(const EntityId& other) const noexcept = default;
};

struct EntitySlot {
    uint32_t generation = 1;
    bool alive = false;
};

struct EntityRegistry {
    std::vector<EntitySlot> slots;
    std::vector<uint32_t> free_list;
};

struct SimulationClock {
    double fixed_dt = 1.0 / 60.0;
    double accumulator = 0.0;
    uint64_t total_ticks = 0;
};

struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct WorldManifest {
    std::string world_name;
    uint32_t sector_count = 0;
    uint32_t initial_entity_count = 0;
};

struct ScriptProgram {
    uint32_t bytecode_version = 0;
    uint32_t entry_offset = 0;
    std::vector<uint8_t> bytecode;
};

struct ScriptVmState {
    uint32_t program_counter = 0;
    uint64_t instructions_executed = 0;
    bool halted = true;
};

EntityId create_entity(EntityRegistry& registry);

bool destroy_entity(EntityRegistry& registry, EntityId entity) noexcept;

bool is_entity_alive(const EntityRegistry& registry, EntityId entity) noexcept;

uint32_t step_fixed_timestep(SimulationClock& clock,
                             double delta_seconds,
                             uint32_t max_substeps = 8) noexcept;

Status load_world_manifest(std::span<const uint8_t> manifest_bytes,
                           WorldManifest& out_manifest);

Status stream_world_sector(const WorldManifest& manifest,
                           Vec3f focus_position,
                           EntityRegistry& registry);

Status step_rigid_body_physics(EntityRegistry& registry, double fixed_dt);

Status load_script_bytecode(std::span<const uint8_t> script_bytes,
                            ScriptProgram& out_program);

Status execute_vm_tick(const ScriptProgram& program,
                       ScriptVmState& vm_state,
                       uint32_t instruction_budget);

Status dispatch_native_call(uint64_t native_hash,
                            std::span<uint64_t> args,
                            uint64_t& out_return_value);

}  // namespace open_vfs::world_vm
