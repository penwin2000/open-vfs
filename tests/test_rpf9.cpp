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

    // 5. Pending stubs respond cleanly
    std::vector<Rpf9TocEntry> entries;
    OVF_EXPECT(parse_rpf9_toc_table(buf, hdr, entries) == Status::NotImplemented);
    Rpf9BlockHeader blk{};
    OVF_EXPECT(parse_rpf9_block_header(buf, blk) == Status::NotImplemented);

    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
