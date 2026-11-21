#include "test_harness.hpp"
#include "vfs_core/vfs_core.hpp"

#include <array>
#include <filesystem>
#include <vector>

int main() {
    using namespace open_vfs;
    using namespace open_vfs::vfs_core;

    // 1. Little-endian readers on synthetic buffer
    const std::array<uint8_t, 8> le_buf{0x78, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x90};
    OVF_EXPECT(read_u16_le(le_buf, 0) == 0x5678u);
    OVF_EXPECT(read_u32_le(le_buf, 0) == 0x12345678u);
    OVF_EXPECT(read_u64_le(le_buf, 0) == 0x90ABCDEF12345678ull);
    OVF_EXPECT(!read_u32_le(le_buf, 6).has_value());
    OVF_EXPECT(!read_u64_le(le_buf, 1).has_value());

    // 2. 16-byte alignment helpers
    OVF_EXPECT(is_aligned_16(0));
    OVF_EXPECT(is_aligned_16(16));
    OVF_EXPECT(is_aligned_16(4096));
    OVF_EXPECT(!is_aligned_16(15));
    OVF_EXPECT(!is_aligned_16(17));
    OVF_EXPECT(align_up_16(0) == 0);
    OVF_EXPECT(align_up_16(1) == 16);
    OVF_EXPECT(align_up_16(16) == 16);
    OVF_EXPECT(align_up_16(17) == 32);

    // 3. Virtual path normalization & joining
    OVF_EXPECT(normalize_virtual_path("data\\packs//./common/../stream/") == "/data/packs/stream");
    OVF_EXPECT(normalize_virtual_path("../../..") == "/");
    OVF_EXPECT(join_virtual_path("/data", "levels/zone_01.rpf") == "/data/levels/zone_01.rpf");
    OVF_EXPECT(join_virtual_path("/data", "/override/root.xvd") == "/override/root.xvd");

    // 4. Mount directory & mount resolution using temporary synthetic directory
    const auto temp_dir = std::filesystem::temp_directory_path() / "open_vfs_test_mount";
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);

    MountRegistry registry;
    OVF_EXPECT(mount_directory(registry, "/data", temp_dir) == Status::Ok);
    OVF_EXPECT(mount_directory(registry, "/missing", temp_dir / "does_not_exist_999") == Status::NotFound);

    const auto match = resolve_mount(registry, "/data/sub/asset.bin");
    OVF_EXPECT(match.has_value() && *match == 0);
    OVF_EXPECT(!resolve_mount(registry, "/other/asset.bin").has_value());

    // 5. Pending stubs return expected clean statuses
    VirtualStreamHandle stream{};
    OVF_EXPECT(open_virtual_stream(registry, "/data/test", stream) == Status::NotImplemented);
    std::array<uint8_t, 16> scratch{};
    std::size_t bytes_read = 0;
    OVF_EXPECT(read_aligned_chunk(stream, 5, scratch, bytes_read) == Status::AlignmentError);
    OVF_EXPECT(read_aligned_chunk(stream, 16, scratch, bytes_read) == Status::NotImplemented);
    std::vector<std::string> listed;
    OVF_EXPECT(enumerate_mounted_entries(registry, "/data", listed) == Status::NotImplemented);

    std::filesystem::remove_all(temp_dir, ec);
    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
