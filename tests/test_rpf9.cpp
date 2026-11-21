#include "test_harness.hpp"
#include "rpf9/rpf9.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

void write_u16_le(std::vector<uint8_t>& buf, std::size_t offset, uint16_t value) {
    buf[offset + 0] = static_cast<uint8_t>(value & 0xFFu);
    buf[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
}

std::vector<uint8_t> make_valid_rpf9_header_bytes(uint32_t region_b_offset,
                                                  uint32_t metadata_end_offset,
                                                  uint32_t tail_table_size,
                                                  uint32_t entry_count,
                                                  uint16_t region_b_mode = open_vfs::rpf9::kRpf9RegionBModeCompact,
                                                  uint16_t archive_flags = 0x0006u,
                                                  uint64_t archive_seed = 0x1122334455667788ull,
                                                  uint32_t key_profile_tag = 0xD98004D5u) {
    using namespace open_vfs::rpf9;
    std::vector<uint8_t> buf(kRpf9HeaderSize, 0);
    open_vfs::test::write_u32_le(buf, 0x00, kRpf9Magic);
    open_vfs::test::write_u32_le(buf, 0x04, metadata_end_offset);
    open_vfs::test::write_u32_le(buf, 0x08, region_b_offset);
    open_vfs::test::write_u32_le(buf, 0x0C, tail_table_size);
    open_vfs::test::write_u64_le(buf, 0x10, archive_seed);
    open_vfs::test::write_u32_le(buf, 0x18, kRpf9FormatConstant);
    open_vfs::test::write_u32_le(buf, 0x1C, kRpf9RegionAOffset);
    write_u16_le(buf, 0x20, archive_flags);
    write_u16_le(buf, 0x22, region_b_mode);
    open_vfs::test::write_u32_le(buf, 0x24, entry_count);
    open_vfs::test::write_u32_le(buf, 0x28, key_profile_tag);
    buf[0x2C] = expected_rpf9_index_width(entry_count);
    buf[0x2D] = kRpf9HeaderByte2D;
    buf[0x2E] = kRpf9HeaderByte2E;
    buf[0x2F] = kRpf9HeaderByte2F;
    open_vfs::test::write_u32_le(buf, 0x30, kRpf9HeaderAlignment);
    // +0x34..+0x3F remain zero reserved padding
    return buf;
}

class TestXorDecryptor final : public open_vfs::rpf9::Rpf9Decryptor {
public:
    open_vfs::Status decrypt_region(const open_vfs::rpf9::Rpf9Header& header,
                                    std::span<const uint8_t> crypto_block,
                                    open_vfs::rpf9::Rpf9RegionKind region,
                                    std::span<uint8_t> buffer_in_out) const override {
        if (crypto_block.size() != open_vfs::rpf9::kRpf9CryptoBlockSize) {
            return open_vfs::Status::InvalidArgument;
        }
        const uint8_t mask = static_cast<uint8_t>(
            (header.archive_seed & 0xFFu) ^
            crypto_block[0] ^
            static_cast<uint8_t>(region));
        for (uint8_t& byte : buffer_in_out) {
            byte ^= mask;
        }
        return open_vfs::Status::Ok;
    }
};

}  // namespace

int main() {
    using namespace open_vfs;
    using namespace open_vfs::rpf9;

    // 1. Magic validation for verified LE u32 0x52504639 (on-disk bytes 39 46 50 52)
    OVF_EXPECT(validate_rpf9_magic(0x52504639u));
    OVF_EXPECT(!validate_rpf9_magic(0x39465052u));  // Byte-swapped ASCII order is rejected
    OVF_EXPECT(!validate_rpf9_magic(0x12345678u));
    OVF_EXPECT(!validate_rpf9_magic(0u));

    // 2. Valid 64-byte plaintext headers: entry_count <= 255 (index_width == 1)
    //    and entry_count >= 256 (index_width == 2), plus nested region_b_mode == 2
    {
        // 2a. entry_count == 0 -> index_width == 1, expected Region B size == 0
        OVF_EXPECT(expected_rpf9_index_width(0u) == 1u);
        OVF_EXPECT(expected_rpf9_region_b_size(0u, 1u) == 0ull);
        const auto zero_buf = make_valid_rpf9_header_bytes(0x150u, 0x150u, 0x0u, 0u);
        OVF_EXPECT(zero_buf[0] == 0x39u && zero_buf[1] == 0x46u &&
                   zero_buf[2] == 0x50u && zero_buf[3] == 0x52u);
        Rpf9Header zero_hdr{};
        OVF_EXPECT(parse_rpf9_header(zero_buf, zero_hdr) == Status::Ok);
        OVF_EXPECT(zero_hdr.entry_count == 0u);
        OVF_EXPECT(zero_hdr.index_width == 1u);

        // 2b. Boundary entry_count == 255 -> index_width == 1, Region B == ALIGN_UP(255 * 9, 16) = 0x900
        OVF_EXPECT(expected_rpf9_index_width(255u) == 1u);
        OVF_EXPECT(expected_rpf9_region_b_size(255u, 1u) == 0x900ull);
        const auto small_buf = make_valid_rpf9_header_bytes(
            0x0600u, 0x0600u + 0x0900u, 0x0240u, 255u,
            kRpf9RegionBModeCompact, 0x0005u, 0xCAFEBABE01020304ull, 0xDA72573Eu);
        Rpf9Header small_hdr{};
        OVF_EXPECT(parse_rpf9_header(small_buf, small_hdr) == Status::Ok);
        OVF_EXPECT(small_hdr.magic == kRpf9Magic);
        OVF_EXPECT(small_hdr.metadata_end_offset == 0x0F00u);
        OVF_EXPECT(small_hdr.region_b_offset == 0x0600u);
        OVF_EXPECT(small_hdr.tail_table_size == 0x0240u);
        OVF_EXPECT(small_hdr.archive_seed == 0xCAFEBABE01020304ull);
        OVF_EXPECT(small_hdr.format_constant == kRpf9FormatConstant);
        OVF_EXPECT(small_hdr.region_a_offset == kRpf9RegionAOffset);
        OVF_EXPECT(small_hdr.archive_flags == 0x0005u);
        OVF_EXPECT(small_hdr.region_b_mode == kRpf9RegionBModeCompact);
        OVF_EXPECT(small_hdr.entry_count == 255u);
        OVF_EXPECT(small_hdr.key_profile_tag == 0xDA72573Eu);
        OVF_EXPECT(small_hdr.index_width == 1u);
        OVF_EXPECT(small_hdr.header_byte_2d == 0x0Fu);
        OVF_EXPECT(small_hdr.header_byte_2e == 0x09u);
        OVF_EXPECT(small_hdr.header_byte_2f == 0x00u);
        OVF_EXPECT(small_hdr.alignment == 0x10u);

        // 2c. Boundary entry_count == 256 -> index_width == 2, Region B == ALIGN_UP(256 * 10, 16) = 0xA00
        OVF_EXPECT(expected_rpf9_index_width(256u) == 2u);
        OVF_EXPECT(expected_rpf9_region_b_size(256u, 2u) == 0xA00ull);
        const auto large_buf = make_valid_rpf9_header_bytes(
            0x3C00u, 0x3C00u + 0x0A00u, 0x2A90u, 256u,
            kRpf9RegionBModeCompact, 0x0006u, 0x0102030405060708ull, 0xD98004D5u);
        Rpf9Header large_hdr{};
        OVF_EXPECT(parse_rpf9_header(large_buf, large_hdr) == Status::Ok);
        OVF_EXPECT(large_hdr.entry_count == 256u);
        OVF_EXPECT(large_hdr.index_width == 2u);
        OVF_EXPECT(large_hdr.metadata_end_offset == 0x4600u);
        OVF_EXPECT(large_hdr.region_b_offset == 0x3C00u);
        OVF_EXPECT(large_hdr.tail_table_size == 0x2A90u);

        // 2d. Nested archive header with region_b_mode == 2
        const auto mode2_buf = make_valid_rpf9_header_bytes(
            0xEF90u, 0x11F10u, 0x62B0u, 1012u,
            kRpf9RegionBModeExtended, 0x0006u, 0xAAAABBBBCCCCDDDDull, 0xC0A92754u);
        Rpf9Header mode2_hdr{};
        OVF_EXPECT(parse_rpf9_header(mode2_buf, mode2_hdr) == Status::Ok);
        OVF_EXPECT(mode2_hdr.region_b_mode == kRpf9RegionBModeExtended);
        OVF_EXPECT(mode2_hdr.entry_count == 1012u);
        OVF_EXPECT(mode2_hdr.index_width == 2u);
    }

    // 3. Header validation error paths (truncation, magic, alignment, constants, index_width, padding)
    {
        const auto base_buf = make_valid_rpf9_header_bytes(0x0340u, 0x0360u, 0x0040u, 2u);
        Rpf9Header sentinel_hdr{};
        OVF_EXPECT(parse_rpf9_header(base_buf, sentinel_hdr) == Status::Ok);

        // Truncated buffers (< 64 bytes) -> OutOfBounds, out_header untouched
        Rpf9Header untouched = sentinel_hdr;
        OVF_EXPECT(parse_rpf9_header(std::span<const uint8_t>(base_buf.data(), 0), untouched) == Status::OutOfBounds);
        OVF_EXPECT(parse_rpf9_header(std::span<const uint8_t>(base_buf.data(), 32), untouched) == Status::OutOfBounds);
        OVF_EXPECT(parse_rpf9_header(std::span<const uint8_t>(base_buf.data(), 63), untouched) == Status::OutOfBounds);
        OVF_EXPECT(untouched.magic == kRpf9Magic && untouched.entry_count == 2u);

        // Bad magic (arbitrary and byte-swapped)
        {
            auto bad = base_buf;
            test::write_u32_le(bad, 0x00, 0xDEADBEEFu);
            Rpf9Header hdr{};
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::InvalidMagic);
            test::write_u32_le(bad, 0x00, 0x39465052u);
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::InvalidMagic);
        }

        // Unaligned offsets (+0x04, +0x08, +0x0C) and bad alignment field (+0x30)
        for (const std::size_t off : {std::size_t{0x04}, std::size_t{0x08}, std::size_t{0x0C}}) {
            auto bad = base_buf;
            bad[off] |= 0x04u;
            Rpf9Header hdr{};
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::AlignmentError);
        }
        for (const uint32_t bad_align : {0u, 8u, 0x20u, 0x100u}) {
            auto bad = base_buf;
            test::write_u32_le(bad, 0x30, bad_align);
            Rpf9Header hdr{};
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::AlignmentError);
        }

        // Bad format_constant (+0x18)
        {
            auto bad = base_buf;
            test::write_u32_le(bad, 0x18, 0x00047A01u);
            Rpf9Header hdr{};
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::UnsupportedFormat);
        }

        // Bad region_a_offset (+0x1C)
        for (const uint32_t bad_reg_a : {0x00000040u, 0x00000140u, 0x00000154u, 0x00000200u}) {
            auto bad = base_buf;
            test::write_u32_le(bad, 0x1C, bad_reg_a);
            Rpf9Header hdr{};
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::CorruptData);
        }

        // Bad region_b_mode (+0x22)
        for (const uint16_t bad_mode : {uint16_t{0u}, uint16_t{3u}, uint16_t{0xFFFFu}}) {
            auto bad = base_buf;
            write_u16_le(bad, 0x22, bad_mode);
            Rpf9Header hdr{};
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::UnsupportedFormat);
        }

        // Mismatched index_width (+0x2C) against entry_count (+0x24)
        {
            auto bad = make_valid_rpf9_header_bytes(0x0340u, 0x0360u, 0x0040u, 255u);
            bad[0x2C] = 2u;
            Rpf9Header hdr{};
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::CorruptData);

            bad[0x2C] = 0u;
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::CorruptData);

            auto bad256 = make_valid_rpf9_header_bytes(0x0600u, 0x1000u, 0x0040u, 256u);
            bad256[0x2C] = 1u;
            OVF_EXPECT(parse_rpf9_header(bad256, hdr) == Status::CorruptData);

            bad256[0x2C] = 3u;
            OVF_EXPECT(parse_rpf9_header(bad256, hdr) == Status::CorruptData);
        }

        // Bad constant header bytes at +0x2D, +0x2E, +0x2F
        {
            auto bad = base_buf;
            bad[0x2D] = 0x0Eu;
            Rpf9Header hdr{};
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::UnsupportedFormat);

            bad = base_buf;
            bad[0x2E] = 0x08u;
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::UnsupportedFormat);

            bad = base_buf;
            bad[0x2F] = 0x01u;
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::UnsupportedFormat);
        }

        // Non-zero reserved padding at every byte in [0x34..0x3F]
        for (std::size_t off = kRpf9ReservedPaddingOffset; off < kRpf9HeaderSize; ++off) {
            auto bad = base_buf;
            bad[off] = 0x01u;
            Rpf9Header hdr{};
            OVF_EXPECT(parse_rpf9_header(bad, hdr) == Status::CorruptData);
        }
    }

    // 4. Region A / B / C & Payload layout validator (validate_rpf9_regions)
    {
        // 4a. Compact mode 1 with entry_count <= 255 (index_width == 1):
        //     entry_count = 29 -> ALIGN_UP(29 * 9, 16) = ALIGN_UP(261, 16) = 272 = 0x110
        const auto buf_small = make_valid_rpf9_header_bytes(0x0670u, 0x0780u, 0x0480u, 29u);
        Rpf9Header hdr_small{};
        OVF_EXPECT(parse_rpf9_header(buf_small, hdr_small) == Status::Ok);

        Rpf9RegionLayout layout{};
        constexpr uint64_t kSmallFileSize = 0x10000ull;
        OVF_EXPECT(validate_rpf9_regions(hdr_small, kSmallFileSize, layout) == Status::Ok);
        OVF_EXPECT(layout.crypto_block == (Rpf9ByteRange{0x40ull, 272ull}));
        OVF_EXPECT(layout.crypto_block.end_offset() == 0x150ull);
        OVF_EXPECT(layout.region_a == (Rpf9ByteRange{0x150ull, 0x0670ull - 0x150ull}));
        OVF_EXPECT(layout.region_b == (Rpf9ByteRange{0x0670ull, 0x0110ull}));
        OVF_EXPECT(layout.payload == (Rpf9ByteRange{0x0780ull, kSmallFileSize - 0x0480ull - 0x0780ull}));
        OVF_EXPECT(layout.region_c == (Rpf9ByteRange{kSmallFileSize - 0x0480ull, 0x0480ull}));
        OVF_EXPECT(layout.region_c.end_offset() == kSmallFileSize);

        // 4b. Compact mode 1 with entry_count >= 256 (index_width == 2) & > 4 GiB archive size:
        //     entry_count = 265 -> ALIGN_UP(265 * 10, 16) = ALIGN_UP(2650, 16) = 2656 = 0xA60
        const auto buf_large = make_valid_rpf9_header_bytes(0x3C00u, 0x4660u, 0x2A90u, 265u);
        Rpf9Header hdr_large{};
        OVF_EXPECT(parse_rpf9_header(buf_large, hdr_large) == Status::Ok);

        constexpr uint64_t kLargeFileSize = 0x1'2000'0000ull;  // 4.5 GiB
        Rpf9RegionLayout large_layout{};
        OVF_EXPECT(validate_rpf9_regions(hdr_large, kLargeFileSize, large_layout) == Status::Ok);
        OVF_EXPECT(large_layout.region_a == (Rpf9ByteRange{0x150ull, 0x3AB0ull}));
        OVF_EXPECT(large_layout.region_b == (Rpf9ByteRange{0x3C00ull, 0x0A60ull}));
        OVF_EXPECT(large_layout.payload.offset == 0x4660ull);
        OVF_EXPECT(large_layout.payload.end_offset() == kLargeFileSize - 0x2A90ull);
        OVF_EXPECT(large_layout.region_c == (Rpf9ByteRange{kLargeFileSize - 0x2A90ull, 0x2A90ull}));

        // 4c. Minimal empty archive at exact boundary (file_size == 0x150)
        const auto buf_min = make_valid_rpf9_header_bytes(0x150u, 0x150u, 0u, 0u);
        Rpf9Header hdr_min{};
        OVF_EXPECT(parse_rpf9_header(buf_min, hdr_min) == Status::Ok);
        Rpf9RegionLayout min_layout{};
        OVF_EXPECT(validate_rpf9_regions(hdr_min, 0x150ull, min_layout) == Status::Ok);
        OVF_EXPECT(min_layout.crypto_block == (Rpf9ByteRange{0x40ull, 272ull}));
        OVF_EXPECT(min_layout.region_a.empty());
        OVF_EXPECT(min_layout.region_b.empty());
        OVF_EXPECT(min_layout.payload.empty());
        OVF_EXPECT(min_layout.region_c.empty());

        // 4d. Region B formula enforcement in mode 1 (off by +0x10 or -0x10 rejected)
        Rpf9Header bad_rb_plus = hdr_small;
        bad_rb_plus.metadata_end_offset += 0x10u;
        OVF_EXPECT(validate_rpf9_regions(bad_rb_plus, kSmallFileSize, layout) == Status::CorruptData);
        OVF_EXPECT(layout.crypto_block.empty());

        Rpf9Header bad_rb_minus = hdr_small;
        bad_rb_minus.metadata_end_offset -= 0x10u;
        OVF_EXPECT(validate_rpf9_regions(bad_rb_minus, kSmallFileSize, layout) == Status::CorruptData);

        // 4e. Region B mode 2 tolerance (non-compact Region B size accepted when bounds hold)
        const auto buf_mode2 = make_valid_rpf9_header_bytes(
            0xEF90u, 0x11F10u, 0x62B0u, 1012u, kRpf9RegionBModeExtended);
        Rpf9Header hdr_mode2{};
        OVF_EXPECT(parse_rpf9_header(buf_mode2, hdr_mode2) == Status::Ok);
        OVF_EXPECT((0x11F10u - 0xEF90u) != expected_rpf9_region_b_size(1012u, 2u));

        Rpf9RegionLayout mode2_layout{};
        constexpr uint64_t kMode2FileSize = 0x40000ull;
        OVF_EXPECT(validate_rpf9_regions(hdr_mode2, kMode2FileSize, mode2_layout) == Status::Ok);
        OVF_EXPECT(mode2_layout.region_a == (Rpf9ByteRange{0x150ull, 0xEF90ull - 0x150ull}));
        OVF_EXPECT(mode2_layout.region_b == (Rpf9ByteRange{0xEF90ull, 0x2F80ull}));
        OVF_EXPECT(mode2_layout.payload == (Rpf9ByteRange{0x11F10ull, kMode2FileSize - 0x62B0ull - 0x11F10ull}));
        OVF_EXPECT(mode2_layout.region_c == (Rpf9ByteRange{kMode2FileSize - 0x62B0ull, 0x62B0ull}));

        // 4f. Ordering errors: region_b_offset < 0x150 or metadata_end_offset < region_b_offset
        Rpf9Header bad_order_a = hdr_small;
        bad_order_a.region_b_offset = 0x140u;
        OVF_EXPECT(validate_rpf9_regions(bad_order_a, kSmallFileSize, layout) == Status::CorruptData);

        Rpf9Header bad_order_b = hdr_mode2;
        bad_order_b.metadata_end_offset = bad_order_b.region_b_offset - 0x10u;
        OVF_EXPECT(validate_rpf9_regions(bad_order_b, kMode2FileSize, layout) == Status::CorruptData);

        // 4g. Truncation and overflow-safe bounds checks
        // tail_table_size > file_size (underflow guard)
        OVF_EXPECT(validate_rpf9_regions(hdr_small, 0x0470ull, layout) == Status::OutOfBounds);
        // file_size == metadata_end + tail_table_size -> valid with empty payload
        OVF_EXPECT(validate_rpf9_regions(hdr_small, 0x0780ull + 0x0480ull, layout) == Status::Ok);
        OVF_EXPECT(layout.payload.empty());
        // Truncated by 1 byte -> OutOfBounds
        OVF_EXPECT(validate_rpf9_regions(hdr_small, 0x0780ull + 0x0480ull - 1ull, layout) == Status::OutOfBounds);

        // 32-bit addition overflow trap: metadata_end + tail_size wraps u32 if summed naively
        Rpf9Header wrap_hdr = hdr_mode2;
        wrap_hdr.region_b_offset = 0xFFFF'E000u;
        wrap_hdr.metadata_end_offset = 0xFFFF'F000u;
        wrap_hdr.tail_table_size = 0x0000'2000u;
        OVF_EXPECT(validate_rpf9_regions(wrap_hdr, 0xFFFF'F000ull, layout) == Status::OutOfBounds);
        // With sufficient 64-bit file_size, the same header is valid
        OVF_EXPECT(validate_rpf9_regions(wrap_hdr, 0x1'0000'1000ull, layout) == Status::Ok);
        OVF_EXPECT(layout.payload.empty());
        OVF_EXPECT(layout.region_c == (Rpf9ByteRange{0xFFFF'F000ull, 0x2000ull}));

        // Extreme uint64_t::max() file_size does not overflow
        OVF_EXPECT(validate_rpf9_regions(hdr_small, std::numeric_limits<uint64_t>::max(), layout) == Status::Ok);
        OVF_EXPECT(layout.region_c.end_offset() == std::numeric_limits<uint64_t>::max());

        // Header invariant re-checks inside validate_rpf9_regions
        Rpf9Header bad_magic = hdr_small;
        bad_magic.magic = 0x11223344u;
        OVF_EXPECT(validate_rpf9_regions(bad_magic, kSmallFileSize, layout) == Status::InvalidMagic);

        Rpf9Header bad_align = hdr_small;
        bad_align.tail_table_size = 0x0484u;
        OVF_EXPECT(validate_rpf9_regions(bad_align, kSmallFileSize, layout) == Status::AlignmentError);

        Rpf9Header bad_fmt = hdr_small;
        bad_fmt.format_constant = 0u;
        OVF_EXPECT(validate_rpf9_regions(bad_fmt, kSmallFileSize, layout) == Status::UnsupportedFormat);

        Rpf9Header bad_iw = hdr_small;
        bad_iw.index_width = 2u;
        OVF_EXPECT(validate_rpf9_regions(bad_iw, kSmallFileSize, layout) == Status::CorruptData);
    }

    // 5. Clean-room abstract Rpf9Decryptor interface polymorphic hook test
    {
        const auto hdr_bytes = make_valid_rpf9_header_bytes(0x0200u, 0x0220u, 0x0040u, 2u);
        Rpf9Header hdr{};
        OVF_EXPECT(parse_rpf9_header(hdr_bytes, hdr) == Status::Ok);

        std::vector<uint8_t> crypto_block(kRpf9CryptoBlockSize, 0x5Au);
        std::vector<uint8_t> region_a = {0x10u, 0x20u, 0x30u, 0x40u};
        const std::vector<uint8_t> original = region_a;

        TestXorDecryptor impl;
        const Rpf9Decryptor& decryptor = impl;
        OVF_EXPECT(decryptor.decrypt_region(hdr, crypto_block, Rpf9RegionKind::RegionA, region_a) == Status::Ok);
        OVF_EXPECT(region_a != original);
        OVF_EXPECT(decryptor.decrypt_region(hdr, crypto_block, Rpf9RegionKind::RegionA, region_a) == Status::Ok);
        OVF_EXPECT(region_a == original);

        std::vector<uint8_t> short_crypto(16, 0u);
        OVF_EXPECT(decryptor.decrypt_region(hdr, short_crypto, Rpf9RegionKind::RegionB, region_a) ==
                   Status::InvalidArgument);
    }

    // 6. Plaintext payload scanner (scan_rpf9_plaintext_payloads)
    {
        // Archive layout:
        //   [0x0000, 0x0040): 64-byte header
        //   [0x0040, 0x0150): 272-byte crypto block
        //   [0x0150, 0x03E0): Region A
        //   [0x03E0, 0x0400): Region B (entry_count=2, index_width=1 -> ALIGN_UP(18, 16) = 0x20)
        //   [0x0400, 0x4200): Payload Region
        //   [0x4200, 0x4300): EOF Region C (tail_table_size = 0x100)
        constexpr std::size_t kArchiveSize = 0x4300u;
        std::vector<uint8_t> archive(kArchiveSize, 0u);

        const auto hdr_bytes = make_valid_rpf9_header_bytes(0x03E0u, 0x0400u, 0x0100u, 2u);
        for (std::size_t i = 0; i < hdr_bytes.size(); ++i) {
            archive[i] = hdr_bytes[i];
        }

        // Decoys outside Payload Region (must NOT be reported)
        archive[0x0040u] = 0x78u;  // Inside crypto block
        archive[0x0041u] = 0xDAu;
        archive[0x0150u] = 0x78u;  // Inside Region A
        archive[0x0151u] = 0xDAu;
        archive[0x03E0u] = 0x78u;  // Inside Region B
        archive[0x03E1u] = 0xDAu;
        archive[0x4200u] = 0x78u;  // Inside EOF Region C
        archive[0x4201u] = 0xDAu;

        // Valid aligned streams inside Payload Region [0x0400, 0x4200):
        //   0x0400: 0x10-aligned zlib 78 DA right at payload start
        archive[0x0400u] = 0x78u;
        archive[0x0401u] = 0xDAu;

        //   0x0450: 0x10-aligned zlib 78 DA
        archive[0x0450u] = 0x78u;
        archive[0x0451u] = 0xDAu;

        //   0x1000: 0x1000-aligned nested RPF9 (full valid 64-byte nested header)
        const auto nested_hdr_bytes = make_valid_rpf9_header_bytes(0x0200u, 0x0220u, 0x0040u, 2u);
        for (std::size_t i = 0; i < nested_hdr_bytes.size(); ++i) {
            archive[0x1000u + i] = nested_hdr_bytes[i];
        }

        //   0x2000: 0x1000-aligned SGD5 (bytes 53 47 44 35)
        archive[0x2000u] = 0x53u;
        archive[0x2001u] = 0x47u;
        archive[0x2002u] = 0x44u;
        archive[0x2003u] = 0x35u;

        //   0x3000: 0x1000-aligned ADAT (bytes 41 44 41 54)
        archive[0x3000u] = 0x41u;
        archive[0x3001u] = 0x44u;
        archive[0x3002u] = 0x41u;
        archive[0x3003u] = 0x54u;

        //   0x4000: 0x1000-aligned zlib 78 DA (also 0x10-aligned)
        archive[0x4000u] = 0x78u;
        archive[0x4001u] = 0xDAu;

        //   0x41F0: 0x10-aligned zlib 78 DA at final 16-byte slot of payload
        archive[0x41F0u] = 0x78u;
        archive[0x41F1u] = 0xDAu;

        // Misaligned decoys inside Payload Region (must NOT be reported)
        test::write_u32_le(archive, 0x1010u, kRpf9Magic);  // 0x10-aligned, NOT 0x1000-aligned
        test::write_u32_le(archive, 0x2020u, kSgd5Magic);  // 0x10-aligned, NOT 0x1000-aligned
        test::write_u32_le(archive, 0x3030u, kAdatMagic);  // 0x10-aligned, NOT 0x1000-aligned
        archive[0x1044u] = 0x78u;                          // 4-byte aligned, NOT 0x10-aligned
        archive[0x1045u] = 0xDAu;
        archive[0x105Fu] = 0x78u;                          // unaligned across 0x10 boundary
        archive[0x1060u] = 0xDAu;

        Rpf9Header hdr{};
        OVF_EXPECT(parse_rpf9_header(archive, hdr) == Status::Ok);

        std::vector<Rpf9PlaintextPayloadHit> hits;
        hits.push_back(Rpf9PlaintextPayloadHit{0x9999ull, Rpf9PlaintextKind::Adat});  // stale entry
        OVF_EXPECT(scan_rpf9_plaintext_payloads(archive, hdr, hits) == Status::Ok);

        const std::vector<Rpf9PlaintextPayloadHit> expected_hits = {
            {0x0400ull, Rpf9PlaintextKind::Zlib78Da},
            {0x0450ull, Rpf9PlaintextKind::Zlib78Da},
            {0x1000ull, Rpf9PlaintextKind::NestedRpf9},
            {0x2000ull, Rpf9PlaintextKind::Sgd5},
            {0x3000ull, Rpf9PlaintextKind::Adat},
            {0x4000ull, Rpf9PlaintextKind::Zlib78Da},
            {0x41F0ull, Rpf9PlaintextKind::Zlib78Da},
        };
        OVF_EXPECT(hits == expected_hits);

        // Confirm the detected nested RPF9 payload parses cleanly as an Rpf9Header
        Rpf9Header nested_parsed{};
        const std::span<const uint8_t> nested_slice(archive.data() + 0x1000u, kRpf9HeaderSize);
        OVF_EXPECT(parse_rpf9_header(nested_slice, nested_parsed) == Status::Ok);
        OVF_EXPECT(nested_parsed.entry_count == 2u);

        // Boundary check: signature straddling Payload / Region C boundary must NOT bleed across
        {
            // Payload [0x0400, 0x1003), Region C [0x1003, 0x1013) (tail_table_size = 0x10)
            std::vector<uint8_t> straddle_rpf(0x1013u, 0u);
            const auto shdr = make_valid_rpf9_header_bytes(0x03E0u, 0x0400u, 0x0010u, 2u);
            for (std::size_t i = 0; i < shdr.size(); ++i) {
                straddle_rpf[i] = shdr[i];
            }
            // Write 39 46 50 52 across 0x1000..0x1003 (byte 0x1003 is inside Region C!)
            test::write_u32_le(straddle_rpf, 0x1000u, kRpf9Magic);
            Rpf9Header parsed_shdr{};
            OVF_EXPECT(parse_rpf9_header(straddle_rpf, parsed_shdr) == Status::Ok);
            OVF_EXPECT(scan_rpf9_plaintext_payloads(straddle_rpf, parsed_shdr, hits) == Status::Ok);
            OVF_EXPECT(hits.empty());

            // Payload [0x0400, 0x1001), Region C [0x1001, 0x1011): 78 at 0x1000, DA at 0x1001 (in Region C)
            std::vector<uint8_t> straddle_zlib(0x1011u, 0u);
            for (std::size_t i = 0; i < shdr.size(); ++i) {
                straddle_zlib[i] = shdr[i];
            }
            straddle_zlib[0x1000u] = 0x78u;
            straddle_zlib[0x1001u] = 0xDAu;
            OVF_EXPECT(scan_rpf9_plaintext_payloads(straddle_zlib, parsed_shdr, hits) == Status::Ok);
            OVF_EXPECT(hits.empty());

            // Payload [0x0400, 0x1002), Region C [0x1002, 0x1012): 78 DA fits completely in payload
            std::vector<uint8_t> exact_zlib(0x1012u, 0u);
            for (std::size_t i = 0; i < shdr.size(); ++i) {
                exact_zlib[i] = shdr[i];
            }
            exact_zlib[0x1000u] = 0x78u;
            exact_zlib[0x1001u] = 0xDAu;
            OVF_EXPECT(scan_rpf9_plaintext_payloads(exact_zlib, parsed_shdr, hits) == Status::Ok);
            OVF_EXPECT(hits.size() == 1u);
            OVF_EXPECT(hits[0] == (Rpf9PlaintextPayloadHit{0x1000ull, Rpf9PlaintextKind::Zlib78Da}));
        }

        // Empty payload succeeds and clears previous hits
        {
            std::vector<uint8_t> empty_payload(0x0500u, 0u);
            for (std::size_t i = 0; i < hdr_bytes.size(); ++i) {
                empty_payload[i] = hdr_bytes[i];
            }
            hits.push_back(Rpf9PlaintextPayloadHit{0x1000ull, Rpf9PlaintextKind::Sgd5});
            OVF_EXPECT(scan_rpf9_plaintext_payloads(empty_payload, hdr, hits) == Status::Ok);
            OVF_EXPECT(hits.empty());
        }

        // Truncated archive fails with OutOfBounds and clears stale hits
        {
            hits.push_back(Rpf9PlaintextPayloadHit{0x1000ull, Rpf9PlaintextKind::Sgd5});
            const std::span<const uint8_t> truncated(archive.data(), 0x04FFu);
            OVF_EXPECT(scan_rpf9_plaintext_payloads(truncated, hdr, hits) == Status::OutOfBounds);
            OVF_EXPECT(hits.empty());
        }
    }

    // 7. Synthetic RPF9 TOC entry and decrypted TOC table parsing
    {
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

        constexpr std::size_t kTocOffset = kRpf9RegionAOffset;  // 0x150
        constexpr uint32_t kEntryCount = 3u;
        constexpr uint32_t kRegionBOffset = static_cast<uint32_t>(kTocOffset + kEntryCount * kRpf9TocEntrySize);
        constexpr uint32_t kMetadataEnd = kRegionBOffset + 0x20u;  // ALIGN_UP(3 * 9, 16) = 32 = 0x20
        std::vector<uint8_t> archive(kTocOffset + kEntryCount * kRpf9TocEntrySize, 0u);

        const auto hdr_bytes = make_valid_rpf9_header_bytes(kRegionBOffset, kMetadataEnd, 0u, kEntryCount);
        for (std::size_t i = 0; i < hdr_bytes.size(); ++i) {
            archive[i] = hdr_bytes[i];
        }

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

        // Empty TOC at exact end of archive succeeds and clears previous output
        Rpf9Header empty_hdr = table_hdr;
        empty_hdr.entry_count = 0u;
        empty_hdr.region_a_offset = static_cast<uint32_t>(archive.size());
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
        unaligned_hdr.region_a_offset = 0x154u;
        OVF_EXPECT(parse_rpf9_toc_table(archive, unaligned_hdr, entries) == Status::AlignmentError);
        OVF_EXPECT(entries.empty());

        entries.push_back(Rpf9TocEntry{});
        Rpf9Header oob_offset_hdr = table_hdr;
        oob_offset_hdr.entry_count = 0u;
        oob_offset_hdr.region_a_offset = static_cast<uint32_t>(archive.size()) + 16u;
        OVF_EXPECT(parse_rpf9_toc_table(archive, oob_offset_hdr, entries) == Status::OutOfBounds);
        OVF_EXPECT(entries.empty());

        entries.push_back(Rpf9TocEntry{});
        const std::span<const uint8_t> truncated(archive.data(), archive.size() - 1);
        OVF_EXPECT(parse_rpf9_toc_table(truncated, table_hdr, entries) == Status::OutOfBounds);
        OVF_EXPECT(entries.empty());

        entries.push_back(Rpf9TocEntry{});
        Rpf9Header huge_hdr = table_hdr;
        huge_hdr.entry_count = 0xFFFFFFFFu;
        OVF_EXPECT(parse_rpf9_toc_table(archive, huge_hdr, entries) == Status::OutOfBounds);
        OVF_EXPECT(entries.empty());
    }

    // 8. Pending stubs respond cleanly
    {
        const auto dummy = make_valid_rpf9_header_bytes(0x0200u, 0x0220u, 0x0040u, 2u);
        Rpf9BlockHeader blk{};
        OVF_EXPECT(parse_rpf9_block_header(dummy, blk) == Status::NotImplemented);
        std::vector<uint8_t> payload(16, 0u);
        OVF_EXPECT(decrypt_rpf9_payload(0u, payload) == Status::NotImplemented);
    }

    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
