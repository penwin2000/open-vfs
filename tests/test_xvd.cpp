#include "test_harness.hpp"
#include "xvd/xvd.hpp"

#include <algorithm>
#include <vector>

int main() {
    using namespace open_vfs;
    using namespace open_vfs::xvd;

    // 1. Synthetic valid XVD volume header ("msft-xvd" + 0x1DD3014E)
    std::vector<uint8_t> hdr_buf(kXvdHeaderSize, 0);
    std::copy(kXvdSignature.begin(), kXvdSignature.end(), hdr_buf.begin());
    test::write_u32_le(hdr_buf, 8, kXvdMagic);
    test::write_u32_le(hdr_buf, 12, 3u);            // format_version
    test::write_u32_le(hdr_buf, 16, kXvdPageSize);  // page_size = 4096
    test::write_u32_le(hdr_buf, 20, 2u);            // partition_count
    test::write_u64_le(hdr_buf, 24, 1048576ull);    // drive_size
    test::write_u64_le(hdr_buf, 32, 4096ull);       // data_offset
    test::write_u64_le(hdr_buf, 40, 8192ull);       // hash_tree_offset
    test::write_u64_le(hdr_buf, 48, 12288ull);      // userdata_offset

    OVF_EXPECT(validate_xvd_magic(hdr_buf));

    XvdHeader hdr{};
    OVF_EXPECT(parse_xvd_header(hdr_buf, hdr) == Status::Ok);
    OVF_EXPECT(hdr.magic == kXvdMagic);
    OVF_EXPECT(hdr.page_size == 4096u);
    OVF_EXPECT(hdr.partition_count == 2u);
    OVF_EXPECT(hdr.drive_size == 1048576ull);

    // 2. Reject invalid signature
    hdr_buf[0] = 'X';
    OVF_EXPECT(!validate_xvd_magic(hdr_buf));
    OVF_EXPECT(parse_xvd_header(hdr_buf, hdr) == Status::InvalidMagic);

    // 3. Synthetic partition table entry
    std::vector<uint8_t> part_buf(kXvdPartitionEntrySize, 0);
    test::write_u32_le(part_buf, 0, 1u);
    test::write_u32_le(part_buf, 4, 0x10u);
    test::write_u64_le(part_buf, 8, 64ull);
    test::write_u64_le(part_buf, 16, 128ull);
    test::write_u64_le(part_buf, 24, 524288ull);

    XvdPartitionEntry part{};
    OVF_EXPECT(parse_xvd_partition_entry(part_buf, part) == Status::Ok);
    OVF_EXPECT(part.partition_id == 1u);
    OVF_EXPECT(part.start_lba == 64ull);
    OVF_EXPECT(part.byte_length == 524288ull);

    // 4. Pending stubs return NotImplemented
    std::vector<XvdPartitionEntry> parts;
    OVF_EXPECT(enumerate_xvd_partitions(hdr_buf, hdr, parts) == Status::NotImplemented);
    bool valid = true;
    OVF_EXPECT(verify_xvd_hash_tree(hdr_buf, hdr, valid) == Status::NotImplemented);

    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
