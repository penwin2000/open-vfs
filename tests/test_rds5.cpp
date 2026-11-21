#include "test_harness.hpp"
#include "rds5/rds5.hpp"

#include <vector>

int main() {
    using namespace open_vfs;
    using namespace open_vfs::rds5;

    // 1. Magic validation for 0x52445335 ("RDS5")
    OVF_EXPECT(validate_rds5_magic(0x52445335u));
    OVF_EXPECT(validate_rds5_magic(0x35534452u));
    OVF_EXPECT(!validate_rds5_magic(0x00000000u));

    // 2. Synthetic valid RDS5 header buffer
    std::vector<uint8_t> header_buf(kRds5HeaderSize, 0);
    test::write_u32_le(header_buf, 0, kRds5Magic);
    test::write_u32_le(header_buf, 4, 5u);       // version
    test::write_u32_le(header_buf, 8, 12u);      // descriptor_count
    test::write_u32_le(header_buf, 12, 24u);     // chunk_count
    test::write_u64_le(header_buf, 16, 64ull);   // descriptor_table_offset
    test::write_u64_le(header_buf, 24, 512ull);  // stream_table_offset

    Rds5Header hdr{};
    OVF_EXPECT(parse_rds5_header(header_buf, hdr) == Status::Ok);
    OVF_EXPECT(hdr.version == 5u);
    OVF_EXPECT(hdr.descriptor_count == 12u);
    OVF_EXPECT(hdr.chunk_count == 24u);

    // 3. Synthetic resource descriptor
    std::vector<uint8_t> desc_buf(kRds5DescriptorSize, 0);
    test::write_u64_le(desc_buf, 0, 0xAABBCCDDEEFF0011ull);
    test::write_u32_le(desc_buf, 8, 0x54455830u);  // "TEX0"
    test::write_u32_le(desc_buf, 12, 0x02u);
    test::write_u32_le(desc_buf, 16, 4096u);
    test::write_u32_le(desc_buf, 20, 16384u);

    Rds5ResourceDescriptor desc{};
    OVF_EXPECT(parse_rds5_descriptor(desc_buf, desc) == Status::Ok);
    OVF_EXPECT(desc.resource_id == 0xAABBCCDDEEFF0011ull);
    OVF_EXPECT(desc.virtual_size == 4096u);
    OVF_EXPECT(desc.physical_size == 16384u);

    // 4. Pending stubs return NotImplemented
    std::vector<Rds5ResourceDescriptor> all_descs;
    OVF_EXPECT(parse_rds5_descriptor_table(header_buf, hdr, all_descs) == Status::NotImplemented);
    std::vector<Rds5ChunkInfo> chunks;
    OVF_EXPECT(parse_rds5_chunk_index(header_buf, hdr, chunks) == Status::NotImplemented);

    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
