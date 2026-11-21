#include "test_harness.hpp"
#include "rpf9/rpf9.hpp"

#include <vector>

int main() {
    using namespace open_vfs;
    using namespace open_vfs::rpf9;

    // 1. Magic validation for 0x52504639 ("RPF9")
    OVF_EXPECT(validate_rpf9_magic(0x52504639u));
    OVF_EXPECT(validate_rpf9_magic(0x39465052u));
    OVF_EXPECT(!validate_rpf9_magic(0x12345678u));

    // 2. Synthetic valid RPF9 header buffer (32 bytes, 16-byte aligned offsets)
    std::vector<uint8_t> buf(kRpf9HeaderSize, 0);
    test::write_u32_le(buf, 0, kRpf9Magic);
    test::write_u32_le(buf, 4, 42u);     // entry_count
    test::write_u32_le(buf, 8, 512u);    // names_length
    test::write_u32_le(buf, 12, 0x01u);  // encryption_flags
    test::write_u64_le(buf, 16, 32ull);  // toc_offset (16-byte aligned)
    test::write_u64_le(buf, 24, 1024ull);

    Rpf9Header hdr{};
    OVF_EXPECT(parse_rpf9_header(buf, hdr) == Status::Ok);
    OVF_EXPECT(hdr.magic == kRpf9Magic);
    OVF_EXPECT(hdr.entry_count == 42u);
    OVF_EXPECT(hdr.names_length == 512u);
    OVF_EXPECT(hdr.toc_offset == 32ull);
    OVF_EXPECT(hdr.names_offset == 1024ull);

    // 3. Reject unaligned TOC offset & bad magic
    test::write_u64_le(buf, 16, 35ull);
    OVF_EXPECT(parse_rpf9_header(buf, hdr) == Status::AlignmentError);
    test::write_u32_le(buf, 0, 0xDEADBEEFu);
    OVF_EXPECT(parse_rpf9_header(buf, hdr) == Status::InvalidMagic);

    // 4. Synthetic RPF9 TOC entry (directory flag set)
    std::vector<uint8_t> toc_buf(kRpf9TocEntrySize, 0);
    test::write_u32_le(toc_buf, 0, 16u);
    test::write_u32_le(toc_buf, 4, 0x80000001u);
    test::write_u32_le(toc_buf, 8, 4u);
    test::write_u32_le(toc_buf, 12, 10u);

    Rpf9TocEntry entry{};
    OVF_EXPECT(parse_rpf9_toc_entry(toc_buf, entry) == Status::Ok);
    OVF_EXPECT(entry.is_directory);
    OVF_EXPECT(entry.name_offset == 16u);
    OVF_EXPECT(entry.size_or_count == 10u);

    // 5. Synthetic RPF9 TOC table parsing (happy path, 16-byte aligned offset)
    {
        constexpr std::size_t kTocOffset = 48;  // 32-byte header + 16-byte aligned gap
        constexpr uint32_t kEntryCount = 3;
        std::vector<uint8_t> archive(kTocOffset + kEntryCount * kRpf9TocEntrySize, 0);

        test::write_u32_le(archive, 0, kRpf9Magic);
        test::write_u32_le(archive, 4, kEntryCount);
        test::write_u32_le(archive, 8, 64u);
        test::write_u32_le(archive, 12, 0u);
        test::write_u64_le(archive, 16, kTocOffset);
        test::write_u64_le(archive, 24, static_cast<uint64_t>(archive.size()));

        // Entry 0: root directory
        test::write_u32_le(archive, kTocOffset + 0, 0u);
        test::write_u32_le(archive, kTocOffset + 4, 0x80000000u);
        test::write_u32_le(archive, kTocOffset + 8, 1u);
        test::write_u32_le(archive, kTocOffset + 12, 2u);

        // Entry 1: regular file
        test::write_u32_le(archive, kTocOffset + 16, 8u);
        test::write_u32_le(archive, kTocOffset + 20, 0x00000002u);
        test::write_u32_le(archive, kTocOffset + 24, 128u);
        test::write_u32_le(archive, kTocOffset + 28, 4096u);

        // Entry 2: nested directory
        test::write_u32_le(archive, kTocOffset + 32, 24u);
        test::write_u32_le(archive, kTocOffset + 36, 0x80000004u);
        test::write_u32_le(archive, kTocOffset + 40, 3u);
        test::write_u32_le(archive, kTocOffset + 44, 0u);

        Rpf9Header table_hdr{};
        OVF_EXPECT(parse_rpf9_header(archive, table_hdr) == Status::Ok);

        std::vector<Rpf9TocEntry> entries;
        OVF_EXPECT(parse_rpf9_toc_table(archive, table_hdr, entries) == Status::Ok);
        OVF_EXPECT(entries.size() == 3u);

        OVF_EXPECT(entries[0].is_directory);
        OVF_EXPECT(entries[0].name_offset == 0u);
        OVF_EXPECT(entries[0].offset_or_index == 1u);
        OVF_EXPECT(entries[0].size_or_count == 2u);

        OVF_EXPECT(!entries[1].is_directory);
        OVF_EXPECT(entries[1].name_offset == 8u);
        OVF_EXPECT(entries[1].flags == 0x00000002u);
        OVF_EXPECT(entries[1].offset_or_index == 128u);
        OVF_EXPECT(entries[1].size_or_count == 4096u);

        OVF_EXPECT(entries[2].is_directory);
        OVF_EXPECT(entries[2].name_offset == 24u);
        OVF_EXPECT(entries[2].flags == 0x80000004u);
        OVF_EXPECT(entries[2].offset_or_index == 3u);
        OVF_EXPECT(entries[2].size_or_count == 0u);

        // ASCII LE magic variant is also accepted
        Rpf9Header ascii_hdr = table_hdr;
        ascii_hdr.magic = kRpf9MagicAsciiLe;
        OVF_EXPECT(parse_rpf9_toc_table(archive, ascii_hdr, entries) == Status::Ok);
        OVF_EXPECT(entries.size() == 3u);

        // Empty TOC at exact end of archive succeeds and clears previous output
        Rpf9Header empty_hdr = table_hdr;
        empty_hdr.entry_count = 0u;
        empty_hdr.toc_offset = static_cast<uint64_t>(archive.size());
        OVF_EXPECT(parse_rpf9_toc_table(archive, empty_hdr, entries) == Status::Ok);
        OVF_EXPECT(entries.empty());

        // Error paths clear stale output entries
        entries.push_back(Rpf9TocEntry{});
        Rpf9Header bad_magic_hdr = table_hdr;
        bad_magic_hdr.magic = 0x11223344u;
        OVF_EXPECT(parse_rpf9_toc_table(archive, bad_magic_hdr, entries) == Status::InvalidMagic);
        OVF_EXPECT(entries.empty());

        entries.push_back(Rpf9TocEntry{});
        Rpf9Header unaligned_hdr = table_hdr;
        unaligned_hdr.toc_offset = 36ull;
        OVF_EXPECT(parse_rpf9_toc_table(archive, unaligned_hdr, entries) == Status::AlignmentError);
        OVF_EXPECT(entries.empty());

        entries.push_back(Rpf9TocEntry{});
        Rpf9Header oob_offset_hdr = table_hdr;
        oob_offset_hdr.entry_count = 0u;
        oob_offset_hdr.toc_offset = static_cast<uint64_t>(archive.size()) + 16ull;
        OVF_EXPECT(parse_rpf9_toc_table(archive, oob_offset_hdr, entries) == Status::OutOfBounds);
        OVF_EXPECT(entries.empty());

        // Truncated by 1 byte fails with OutOfBounds
        entries.push_back(Rpf9TocEntry{});
        const std::span<const uint8_t> truncated(archive.data(), archive.size() - 1);
        OVF_EXPECT(parse_rpf9_toc_table(truncated, table_hdr, entries) == Status::OutOfBounds);
        OVF_EXPECT(entries.empty());

        // Extreme entry count fails with OutOfBounds
        entries.push_back(Rpf9TocEntry{});
        Rpf9Header huge_hdr = table_hdr;
        huge_hdr.entry_count = 0xFFFFFFFFu;
        OVF_EXPECT(parse_rpf9_toc_table(archive, huge_hdr, entries) == Status::OutOfBounds);
        OVF_EXPECT(entries.empty());
    }

    // 6. Pending stubs respond cleanly
    Rpf9BlockHeader blk{};
    OVF_EXPECT(parse_rpf9_block_header(buf, blk) == Status::NotImplemented);

    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
