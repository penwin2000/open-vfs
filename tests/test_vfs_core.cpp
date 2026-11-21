#include "test_harness.hpp"
#include "vfs_core/vfs_core.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool write_synthetic_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

std::vector<uint8_t> make_rpf9_bytes(bool ascii_le) {
    std::vector<uint8_t> buf(32, 0);
    open_vfs::test::write_u32_le(buf, 0, ascii_le ? 0x39465052u : 0x52504639u);
    return buf;
}

std::vector<uint8_t> make_rds5_bytes(bool ascii_le) {
    std::vector<uint8_t> buf(32, 0);
    open_vfs::test::write_u32_le(buf, 0, ascii_le ? 0x35534452u : 0x52445335u);
    return buf;
}

std::vector<uint8_t> make_xvd_bytes(bool signed_offset_0x200) {
    constexpr std::array<uint8_t, 8> kSig = {'m', 's', 'f', 't', '-', 'x', 'v', 'd'};
    if (signed_offset_0x200) {
        std::vector<uint8_t> buf(0x240, 0xAAu);
        for (std::size_t i = 0; i < kSig.size(); ++i) {
            buf[0x200 + i] = kSig[i];
        }
        return buf;
    }
    std::vector<uint8_t> buf(64, 0);
    for (std::size_t i = 0; i < kSig.size(); ++i) {
        buf[i] = kSig[i];
    }
    open_vfs::test::write_u32_le(buf, 8, 0x1DD3014Eu);
    return buf;
}

std::vector<uint8_t> make_pri_bytes() {
    std::vector<uint8_t> buf(32, 0);
    constexpr std::array<uint8_t, 8> kSig = {'m', 'r', 'm', '_', 'p', 'r', 'i', '2'};
    for (std::size_t i = 0; i < kSig.size(); ++i) {
        buf[i] = kSig[i];
    }
    return buf;
}

void populate_expected_layout(const std::filesystem::path& root, bool signed_xvd) {
    for (std::size_t i = 0; i < open_vfs::vfs_core::kExpectedDataLayout.rpf9_count; ++i) {
        const auto subdir = (i % 2 == 0) ? root : (root / "sub_packs");
        OVF_EXPECT(write_synthetic_file(
            subdir / ("pack_" + std::to_string(i) + ".rpf"), make_rpf9_bytes(i % 2 == 0)));
    }
    for (std::size_t i = 0; i < open_vfs::vfs_core::kExpectedDataLayout.rds5_count; ++i) {
        const auto subdir = (i % 3 == 0) ? (root / "sub_streams") : root;
        OVF_EXPECT(write_synthetic_file(
            subdir / ("stream_" + std::to_string(i) + ".ttpf"), make_rds5_bytes(i % 2 == 0)));
    }
    OVF_EXPECT(write_synthetic_file(root / "volume_00.xvd", make_xvd_bytes(signed_xvd)));
    OVF_EXPECT(write_synthetic_file(root / "resources_00.pri", make_pri_bytes()));
}

}  // namespace

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
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);

    MountRegistry registry;
    OVF_EXPECT(mount_directory(registry, "/data", temp_dir) == Status::Ok);
    OVF_EXPECT(mount_directory(registry, "/missing", temp_dir / "does_not_exist_999") == Status::NotFound);

    const auto match = resolve_mount(registry, "/data/sub/asset.bin");
    OVF_EXPECT(match.has_value() && *match == 0);
    OVF_EXPECT(!resolve_mount(registry, "/other/asset.bin").has_value());

    // 5. Anonymous folder fingerprint validator (verify_data_layout)
    {
        OVF_EXPECT(status_to_string(Status::ErrUnsupportedDataLayout) == "ErrUnsupportedDataLayout");

        DataLayoutFingerprint observed{};
        // Missing directory returns NotFound
        OVF_EXPECT(verify_data_layout(temp_dir / "does_not_exist_layout", &observed) == Status::NotFound);

        // Empty directory returns ErrUnsupportedDataLayout
        const auto empty_dir = temp_dir / "layout_empty";
        std::filesystem::create_directories(empty_dir, ec);
        OVF_EXPECT(verify_data_layout(empty_dir, &observed) == Status::ErrUnsupportedDataLayout);
        const DataLayoutFingerprint empty_expected{};
        OVF_EXPECT(observed == empty_expected);

        // Exact matching synthetic layout (22 RPF9, 57 RDS5, 1 XVD at 0x200, 1 PRI)
        // alongside unrelated non-container files and ignored dot-prefixed folders
        const auto valid_dir = temp_dir / "layout_valid";
        populate_expected_layout(valid_dir, true);
        const std::vector<uint8_t> text_bytes{'h', 'e', 'l', 'l', 'o'};
        const std::vector<uint8_t> bad_rpf_bytes{'B', 'A', 'D', '0', 0, 0, 0, 0};
        OVF_EXPECT(write_synthetic_file(valid_dir / "notes.txt", text_bytes));
        OVF_EXPECT(write_synthetic_file(valid_dir / "spoofed.rpf", bad_rpf_bytes));
        OVF_EXPECT(write_synthetic_file(valid_dir / ".open-vfs-repo" / "ignored.rpf", make_rpf9_bytes(true)));

        OVF_EXPECT(verify_data_layout(valid_dir, &observed) == Status::Ok);
        OVF_EXPECT(observed == kExpectedDataLayout);

        // Also validate XVD header at offset 0x000
        OVF_EXPECT(write_synthetic_file(valid_dir / "volume_00.xvd", make_xvd_bytes(false)));
        OVF_EXPECT(verify_data_layout(valid_dir, &observed) == Status::Ok);
        OVF_EXPECT(observed == kExpectedDataLayout);

        // Extra RPF9 container fails with ErrUnsupportedDataLayout
        const auto extra_rpf9 = valid_dir / "extra_pack.rpf";
        OVF_EXPECT(write_synthetic_file(extra_rpf9, make_rpf9_bytes(true)));
        OVF_EXPECT(verify_data_layout(valid_dir, &observed) == Status::ErrUnsupportedDataLayout);
        OVF_EXPECT(observed.rpf9_count == 23u);
        std::filesystem::remove(extra_rpf9, ec);

        // Corrupting the PRI magic fails with ErrUnsupportedDataLayout even with .pri extension
        const std::vector<uint8_t> bad_pri_bytes{'N', 'O', 'T', '_', 'P', 'R', 'I', '0'};
        OVF_EXPECT(write_synthetic_file(valid_dir / "resources_00.pri", bad_pri_bytes));
        OVF_EXPECT(verify_data_layout(valid_dir, &observed) == Status::ErrUnsupportedDataLayout);
        OVF_EXPECT(observed.pri_count == 0u);
        OVF_EXPECT(write_synthetic_file(valid_dir / "resources_00.pri", make_pri_bytes()));

        // Missing one RDS5 container fails with ErrUnsupportedDataLayout
        std::filesystem::remove(valid_dir / "stream_1.ttpf", ec);
        OVF_EXPECT(verify_data_layout(valid_dir, &observed) == Status::ErrUnsupportedDataLayout);
        OVF_EXPECT(observed.rds5_count == 56u);
    }

    // 6. Pending stubs return expected clean statuses
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
