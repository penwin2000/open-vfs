#include "test_harness.hpp"
#include "rds5/rds5.hpp"

#include <span>
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

    // 4. Synthetic RDS5 descriptor table parsing
    {
        constexpr std::size_t kDescOffset = 48;  // 32-byte header + 16-byte aligned gap
        constexpr uint32_t kDescCount = 3;
        std::vector<uint8_t> container(kDescOffset + kDescCount * kRds5DescriptorSize, 0);

        test::write_u32_le(container, 0, kRds5Magic);
        test::write_u32_le(container, 4, 5u);
        test::write_u32_le(container, 8, kDescCount);
        test::write_u32_le(container, 12, 0u);
        test::write_u64_le(container, 16, kDescOffset);
        test::write_u64_le(container, 24, 128ull);

        // Descriptor 0: texture resource
        const std::size_t d0 = kDescOffset + 0 * kRds5DescriptorSize;
        test::write_u64_le(container, d0 + 0, 0x0102030405060708ull);
        test::write_u32_le(container, d0 + 8, 0x54455830u);  // "TEX0"
        test::write_u32_le(container, d0 + 12, 0x00000001u);
        test::write_u32_le(container, d0 + 16, 4096u);
        test::write_u32_le(container, d0 + 20, 65536u);

        // Descriptor 1: mesh resource (zero physical size)
        const std::size_t d1 = kDescOffset + 1 * kRds5DescriptorSize;
        test::write_u64_le(container, d1 + 0, 0x1122334455667788ull);
        test::write_u32_le(container, d1 + 8, 0x4D534830u);  // "MSH0"
        test::write_u32_le(container, d1 + 12, 0x80000004u);
        test::write_u32_le(container, d1 + 16, 8192u);
        test::write_u32_le(container, d1 + 20, 0u);

        // Descriptor 2: shader resource
        const std::size_t d2 = kDescOffset + 2 * kRds5DescriptorSize;
        test::write_u64_le(container, d2 + 0, 0xFEDCBA9876543210ull);
        test::write_u32_le(container, d2 + 8, 0x53484430u);  // "SHD0"
        test::write_u32_le(container, d2 + 12, 0x00000000u);
        test::write_u32_le(container, d2 + 16, 16384u);
        test::write_u32_le(container, d2 + 20, 32768u);

        Rds5Header table_hdr{};
        OVF_EXPECT(parse_rds5_header(container, table_hdr) == Status::Ok);

        // Happy path: exact-fit 3-descriptor table (120-byte container)
        std::vector<Rds5ResourceDescriptor> all_descs;
        OVF_EXPECT(parse_rds5_descriptor_table(container, table_hdr, all_descs) == Status::Ok);
        OVF_EXPECT(all_descs.size() == 3u);

        OVF_EXPECT(all_descs[0].resource_id == 0x0102030405060708ull);
        OVF_EXPECT(all_descs[0].type_tag == 0x54455830u);
        OVF_EXPECT(all_descs[0].flags == 0x00000001u);
        OVF_EXPECT(all_descs[0].virtual_size == 4096u);
        OVF_EXPECT(all_descs[0].physical_size == 65536u);

        OVF_EXPECT(all_descs[1].resource_id == 0x1122334455667788ull);
        OVF_EXPECT(all_descs[1].type_tag == 0x4D534830u);
        OVF_EXPECT(all_descs[1].flags == 0x80000004u);
        OVF_EXPECT(all_descs[1].virtual_size == 8192u);
        OVF_EXPECT(all_descs[1].physical_size == 0u);

        OVF_EXPECT(all_descs[2].resource_id == 0xFEDCBA9876543210ull);
        OVF_EXPECT(all_descs[2].type_tag == 0x53484430u);
        OVF_EXPECT(all_descs[2].flags == 0x00000000u);
        OVF_EXPECT(all_descs[2].virtual_size == 16384u);
        OVF_EXPECT(all_descs[2].physical_size == 32768u);

        // ASCII LE magic variant ("RDS5" as LE u32) is accepted and replaces stale output
        all_descs.push_back(Rds5ResourceDescriptor{});
        Rds5Header ascii_hdr = table_hdr;
        ascii_hdr.magic = kRds5MagicAsciiLe;
        OVF_EXPECT(parse_rds5_descriptor_table(container, ascii_hdr, all_descs) == Status::Ok);
        OVF_EXPECT(all_descs.size() == 3u);
        OVF_EXPECT(all_descs[2].resource_id == 0xFEDCBA9876543210ull);

        // Offset 0 boundary: standalone descriptor slice without leading header
        Rds5Header zero_off_hdr = table_hdr;
        zero_off_hdr.descriptor_count = 2u;
        zero_off_hdr.descriptor_table_offset = 0ull;
        const std::span<const uint8_t> table_slice(container.data() + kDescOffset,
                                                   2u * kRds5DescriptorSize);
        OVF_EXPECT(parse_rds5_descriptor_table(table_slice, zero_off_hdr, all_descs) == Status::Ok);
        OVF_EXPECT(all_descs.size() == 2u);
        OVF_EXPECT(all_descs[0].resource_id == 0x0102030405060708ull);
        OVF_EXPECT(all_descs[1].resource_id == 0x1122334455667788ull);

        // Empty descriptor table at exact 16-byte aligned end of buffer succeeds and clears output
        std::vector<uint8_t> aligned_container(128, 0);
        Rds5Header empty_end_hdr = table_hdr;
        empty_end_hdr.descriptor_count = 0u;
        empty_end_hdr.descriptor_table_offset = static_cast<uint64_t>(aligned_container.size());
        OVF_EXPECT(parse_rds5_descriptor_table(aligned_container, empty_end_hdr, all_descs) ==
                   Status::Ok);
        OVF_EXPECT(all_descs.empty());

        // Empty descriptor table on empty buffer at offset 0 succeeds
        all_descs.push_back(Rds5ResourceDescriptor{});
        Rds5Header empty_zero_hdr = table_hdr;
        empty_zero_hdr.descriptor_count = 0u;
        empty_zero_hdr.descriptor_table_offset = 0ull;
        OVF_EXPECT(parse_rds5_descriptor_table({}, empty_zero_hdr, all_descs) == Status::Ok);
        OVF_EXPECT(all_descs.empty());

        // Invalid magic clears stale output
        all_descs.push_back(Rds5ResourceDescriptor{});
        Rds5Header bad_magic_hdr = table_hdr;
        bad_magic_hdr.magic = 0xDEADBEEFu;
        OVF_EXPECT(parse_rds5_descriptor_table(container, bad_magic_hdr, all_descs) ==
                   Status::InvalidMagic);
        OVF_EXPECT(all_descs.empty());

        // Unaligned descriptor_table_offset rejected (both non-empty and zero count)
        all_descs.push_back(Rds5ResourceDescriptor{});
        Rds5Header unaligned_hdr = table_hdr;
        unaligned_hdr.descriptor_table_offset = 36ull;
        OVF_EXPECT(parse_rds5_descriptor_table(container, unaligned_hdr, all_descs) ==
                   Status::AlignmentError);
        OVF_EXPECT(all_descs.empty());

        all_descs.push_back(Rds5ResourceDescriptor{});
        Rds5Header unaligned_empty_hdr = table_hdr;
        unaligned_empty_hdr.descriptor_count = 0u;
        unaligned_empty_hdr.descriptor_table_offset = 49ull;
        OVF_EXPECT(parse_rds5_descriptor_table(container, unaligned_empty_hdr, all_descs) ==
                   Status::AlignmentError);
        OVF_EXPECT(all_descs.empty());

        // Out-of-bounds offset past end of container (even with zero count)
        all_descs.push_back(Rds5ResourceDescriptor{});
        Rds5Header oob_offset_hdr = table_hdr;
        oob_offset_hdr.descriptor_count = 0u;
        oob_offset_hdr.descriptor_table_offset = 128ull;  // 16-byte aligned, > 120
        OVF_EXPECT(parse_rds5_descriptor_table(container, oob_offset_hdr, all_descs) ==
                   Status::OutOfBounds);
        OVF_EXPECT(all_descs.empty());

        // Huge aligned offset near UINT64_MAX cannot wrap around
        all_descs.push_back(Rds5ResourceDescriptor{});
        Rds5Header wrap_offset_hdr = table_hdr;
        wrap_offset_hdr.descriptor_count = 1u;
        wrap_offset_hdr.descriptor_table_offset = 0xFFFFFFFFFFFFFFF0ull;
        OVF_EXPECT(parse_rds5_descriptor_table(container, wrap_offset_hdr, all_descs) ==
                   Status::OutOfBounds);
        OVF_EXPECT(all_descs.empty());

        // Truncated by 1 byte fails with OutOfBounds and clears stale output
        all_descs.push_back(Rds5ResourceDescriptor{});
        const std::span<const uint8_t> truncated(container.data(), container.size() - 1);
        OVF_EXPECT(parse_rds5_descriptor_table(truncated, table_hdr, all_descs) ==
                   Status::OutOfBounds);
        OVF_EXPECT(all_descs.empty());

        // Non-zero descriptor count against empty span fails with OutOfBounds
        all_descs.push_back(Rds5ResourceDescriptor{});
        Rds5Header one_desc_hdr = table_hdr;
        one_desc_hdr.descriptor_count = 1u;
        one_desc_hdr.descriptor_table_offset = 0ull;
        OVF_EXPECT(parse_rds5_descriptor_table({}, one_desc_hdr, all_descs) ==
                   Status::OutOfBounds);
        OVF_EXPECT(all_descs.empty());

        // 32-bit multiplication overflow trap: 0x0AAAAAABu * 24 wraps to 8 in uint32_t
        all_descs.push_back(Rds5ResourceDescriptor{});
        Rds5Header overflow32_hdr = table_hdr;
        overflow32_hdr.descriptor_count = 0x0AAAAAABu;
        OVF_EXPECT(parse_rds5_descriptor_table(container, overflow32_hdr, all_descs) ==
                   Status::OutOfBounds);
        OVF_EXPECT(all_descs.empty());

        // Extreme uint32_t max descriptor count fails with OutOfBounds
        all_descs.push_back(Rds5ResourceDescriptor{});
        Rds5Header huge_hdr = table_hdr;
        huge_hdr.descriptor_count = 0xFFFFFFFFu;
        OVF_EXPECT(parse_rds5_descriptor_table(container, huge_hdr, all_descs) ==
                   Status::OutOfBounds);
        OVF_EXPECT(all_descs.empty());
    }

    // 5. Pending stubs return NotImplemented
    std::vector<Rds5ChunkInfo> chunks;
    OVF_EXPECT(parse_rds5_chunk_index(header_buf, hdr, chunks) == Status::NotImplemented);

    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
