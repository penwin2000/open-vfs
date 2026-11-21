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

    // 6. Unknown stream paths fail cleanly; pending read stub returns expected statuses
    VirtualStreamHandle stream{};
    OVF_EXPECT(open_virtual_stream(registry, "/data/test", stream) == Status::NotFound);
    OVF_EXPECT(!stream.valid);
    std::array<uint8_t, 16> scratch{};
    std::size_t bytes_read = 0;
    OVF_EXPECT(read_aligned_chunk(stream, 5, scratch, bytes_read) == Status::AlignmentError);
    OVF_EXPECT(read_aligned_chunk(stream, 16, scratch, bytes_read) == Status::NotImplemented);

    // 7. enumerate_mounted_entries over in-memory synthetic registries
    {
        const auto make_archive = [](std::string virtual_root, ContainerKind kind) {
            MountedArchiveInfo info;
            info.virtual_root = std::move(virtual_root);
            info.kind = kind;
            info.file_size = 64;
            return info;
        };

        MountRegistry synth;
        MountPoint data_mount;
        data_mount.virtual_prefix = "/data";
        data_mount.discovered_archives.push_back(make_archive("/data/pack_b.rpf", ContainerKind::Rpf9));
        data_mount.discovered_archives.push_back(make_archive("/data/pack_a.rpf", ContainerKind::Rpf9));
        data_mount.discovered_archives.push_back(make_archive("/data/stream_0.ttpf", ContainerKind::Rds5));
        synth.mounts.push_back(std::move(data_mount));

        MountPoint nested_mount;
        nested_mount.virtual_prefix = "data\\update";  // unnormalized on purpose
        nested_mount.discovered_archives.push_back(make_archive("/data/update/patch_0.rpf", ContainerKind::Rpf9));
        // Overlaps a path already exposed by /data; must be listed exactly once.
        nested_mount.discovered_archives.push_back(make_archive("/data/pack_a.rpf", ContainerKind::Rpf9));
        synth.mounts.push_back(std::move(nested_mount));

        MountPoint volume_mount;
        volume_mount.virtual_prefix = "/volume";
        volume_mount.discovered_archives.push_back(make_archive("volume//./vol_00.xvd", ContainerKind::Xvd));
        synth.mounts.push_back(std::move(volume_mount));

        MountPoint empty_mount;
        empty_mount.virtual_prefix = "/empty";
        synth.mounts.push_back(std::move(empty_mount));

        std::vector<std::string> listed{"stale"};

        // Mount prefix lists its own archives plus nested mounts, sorted and deduplicated
        OVF_EXPECT(enumerate_mounted_entries(synth, "/data", listed) == Status::Ok);
        const std::vector<std::string> expected_data{
            "/data/pack_a.rpf", "/data/pack_b.rpf", "/data/stream_0.ttpf", "/data/update/patch_0.rpf"};
        OVF_EXPECT(listed == expected_data);

        // Unnormalized prefixes resolve identically
        OVF_EXPECT(enumerate_mounted_entries(synth, "data\\.\\", listed) == Status::Ok);
        OVF_EXPECT(listed == expected_data);

        // Root lists everything across mounts with normalized entry paths
        OVF_EXPECT(enumerate_mounted_entries(synth, "/", listed) == Status::Ok);
        const std::vector<std::string> expected_root{"/data/pack_a.rpf",
                                                     "/data/pack_b.rpf",
                                                     "/data/stream_0.ttpf",
                                                     "/data/update/patch_0.rpf",
                                                     "/volume/vol_00.xvd"};
        OVF_EXPECT(listed == expected_root);
        OVF_EXPECT(enumerate_mounted_entries(synth, "", listed) == Status::Ok);
        OVF_EXPECT(listed == expected_root);

        // Sub-prefix inside a mount narrows to that subtree
        OVF_EXPECT(enumerate_mounted_entries(synth, "/data/update", listed) == Status::Ok);
        OVF_EXPECT(listed.size() == 1u && listed[0] == "/data/update/patch_0.rpf");

        // Covered prefix with no matching entries is an empty Ok listing
        OVF_EXPECT(enumerate_mounted_entries(synth, "/empty", listed) == Status::Ok);
        OVF_EXPECT(listed.empty());
        OVF_EXPECT(enumerate_mounted_entries(synth, "/data/no_such_dir", listed) == Status::Ok);
        OVF_EXPECT(listed.empty());

        // Sibling names sharing a string prefix are not treated as children
        OVF_EXPECT(enumerate_mounted_entries(synth, "/dat", listed) == Status::NotFound);
        OVF_EXPECT(listed.empty());
        OVF_EXPECT(enumerate_mounted_entries(synth, "/data/pack_a", listed) == Status::Ok);
        OVF_EXPECT(listed.empty());

        // A file path itself is not listed as its own child
        OVF_EXPECT(enumerate_mounted_entries(synth, "/volume/vol_00.xvd", listed) == Status::Ok);
        OVF_EXPECT(listed.empty());

        // Prefixes outside every mount, and empty registries, report NotFound
        listed.assign(1, "stale");
        OVF_EXPECT(enumerate_mounted_entries(synth, "/elsewhere", listed) == Status::NotFound);
        OVF_EXPECT(listed.empty());
        const MountRegistry empty_registry;
        OVF_EXPECT(enumerate_mounted_entries(empty_registry, "/", listed) == Status::NotFound);
        OVF_EXPECT(listed.empty());

        // Archives discovered by mount_directory on a synthetic host folder are enumerable
        const auto enum_dir = temp_dir / "enumerate_host";
        OVF_EXPECT(write_synthetic_file(enum_dir / "pack_0.rpf", make_rpf9_bytes(true)));
        OVF_EXPECT(write_synthetic_file(enum_dir / "stream_0.ttpf", make_rds5_bytes(true)));
        OVF_EXPECT(write_synthetic_file(enum_dir / "notes.txt", std::vector<uint8_t>{'x'}));
        MountRegistry host_registry;
        OVF_EXPECT(mount_directory(host_registry, "/game", enum_dir) == Status::Ok);
        OVF_EXPECT(enumerate_mounted_entries(host_registry, "/game", listed) == Status::Ok);
        const std::vector<std::string> expected_host{"/game/pack_0.rpf", "/game/stream_0.ttpf"};
        OVF_EXPECT(listed == expected_host);

        // 8. open_virtual_stream over the same synthetic registries
        VirtualStreamHandle handle{};

        // Archive in the covering mount opens at offset 0 with its recorded size
        OVF_EXPECT(open_virtual_stream(synth, "/data/pack_b.rpf", handle) == Status::Ok);
        OVF_EXPECT(handle.valid);
        OVF_EXPECT(handle.mount_index == 0u && handle.archive_index == 0u);
        OVF_EXPECT(handle.offset == 0u && handle.size == 64u);
        OVF_EXPECT(handle.host_path.empty());

        // Overlapping listing: /data/pack_a.rpf is exposed by /data, not by /data/update
        OVF_EXPECT(open_virtual_stream(synth, "/data/pack_a.rpf", handle) == Status::Ok);
        OVF_EXPECT(handle.mount_index == 0u && handle.archive_index == 1u);

        // Nested mount wins for paths beneath it; unnormalized input resolves identically
        OVF_EXPECT(open_virtual_stream(synth, "/data/update/patch_0.rpf", handle) == Status::Ok);
        OVF_EXPECT(handle.mount_index == 1u && handle.archive_index == 0u);
        OVF_EXPECT(open_virtual_stream(synth, "data\\update\\.\\patch_0.rpf", handle) == Status::Ok);
        OVF_EXPECT(handle.mount_index == 1u && handle.archive_index == 0u);

        // Unnormalized archive virtual roots still match
        OVF_EXPECT(open_virtual_stream(synth, "/volume/vol_00.xvd", handle) == Status::Ok);
        OVF_EXPECT(handle.mount_index == 2u && handle.archive_index == 0u);

        // Mount prefixes, directories, root, and unknown paths are not streams;
        // a previously valid handle is reset on failure
        OVF_EXPECT(handle.valid);
        OVF_EXPECT(open_virtual_stream(synth, "/data", handle) == Status::NotFound);
        OVF_EXPECT(!handle.valid && handle.size == 0u);
        OVF_EXPECT(open_virtual_stream(synth, "/data/update", handle) == Status::NotFound);
        OVF_EXPECT(open_virtual_stream(synth, "/", handle) == Status::NotFound);
        OVF_EXPECT(open_virtual_stream(synth, "", handle) == Status::NotFound);
        OVF_EXPECT(open_virtual_stream(synth, "/data/pack_c.rpf", handle) == Status::NotFound);
        OVF_EXPECT(open_virtual_stream(synth, "/elsewhere/pack_a.rpf", handle) == Status::NotFound);
        OVF_EXPECT(open_virtual_stream(synth, "/empty/pack_a.rpf", handle) == Status::NotFound);
        OVF_EXPECT(open_virtual_stream(empty_registry, "/data/pack_a.rpf", handle) == Status::NotFound);
        OVF_EXPECT(!handle.valid);

        // Traversal collapses inside the virtual namespace
        OVF_EXPECT(open_virtual_stream(synth, "/volume/../data/./pack_b.rpf", handle) == Status::Ok);
        OVF_EXPECT(handle.mount_index == 0u && handle.archive_index == 0u);

        // Host-backed archives carry their host path and on-disk size
        OVF_EXPECT(open_virtual_stream(host_registry, "/game/stream_0.ttpf", handle) == Status::Ok);
        OVF_EXPECT(handle.valid && handle.size == 32u);
        OVF_EXPECT(handle.host_path.filename() == "stream_0.ttpf");
        OVF_EXPECT(open_virtual_stream(host_registry, "/game/notes.txt", handle) == Status::NotFound);

        // A valid handle still honors the read alignment contract
        OVF_EXPECT(open_virtual_stream(host_registry, "/game/pack_0.rpf", handle) == Status::Ok);
        OVF_EXPECT(read_aligned_chunk(handle, 8, scratch, bytes_read) == Status::AlignmentError);
    }

    std::filesystem::remove_all(temp_dir, ec);
    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
