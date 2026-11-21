#include "test_harness.hpp"
#include "world_vm/world_vm.hpp"

#include <array>

int main() {
    using namespace open_vfs;
    using namespace open_vfs::world_vm;

    // 1. Generational entity registry lifecycle
    EntityRegistry reg{};
    const EntityId e0 = create_entity(reg);
    const EntityId e1 = create_entity(reg);
    OVF_EXPECT(is_entity_alive(reg, e0));
    OVF_EXPECT(is_entity_alive(reg, e1));
    OVF_EXPECT(destroy_entity(reg, e0));
    OVF_EXPECT(!is_entity_alive(reg, e0));
    OVF_EXPECT(!destroy_entity(reg, e0));  // double-destroy rejected

    // Recycled slot must carry incremented generation so stale handle stays invalid
    const EntityId e0_reborn = create_entity(reg);
    OVF_EXPECT(e0_reborn.index == e0.index);
    OVF_EXPECT(e0_reborn.generation > e0.generation);
    OVF_EXPECT(!is_entity_alive(reg, e0));
    OVF_EXPECT(is_entity_alive(reg, e0_reborn));

    // 2. Fixed-timestep simulation accumulator
    SimulationClock clock{};
    clock.fixed_dt = 1.0 / 60.0;
    OVF_EXPECT(step_fixed_timestep(clock, 0.05) == 3u);
    OVF_EXPECT(clock.total_ticks == 3ull);
    OVF_EXPECT(step_fixed_timestep(clock, 5.0, 4u) == 4u);  // capped at max_substeps

    // 3. Pending world loader & script VM stubs
    std::array<uint8_t, 32> synthetic_blob{};
    WorldManifest manifest{};
    OVF_EXPECT(load_world_manifest(synthetic_blob, manifest) == Status::NotImplemented);

    ScriptProgram program{};
    ScriptVmState vm{};
    OVF_EXPECT(load_script_bytecode(synthetic_blob, program) == Status::NotImplemented);
    OVF_EXPECT(execute_vm_tick(program, vm, 128u) == Status::NotImplemented);

    std::array<uint64_t, 2> args{1ull, 2ull};
    uint64_t ret = 0;
    OVF_EXPECT(dispatch_native_call(0x1234567890ABCDEFull, args, ret) == Status::NotImplemented);

    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
