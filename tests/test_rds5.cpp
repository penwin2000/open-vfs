#include "test_harness.hpp"
#include "rds5/rds5.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace {

using namespace open_vfs;
using namespace open_vfs::rds5;

void write_rds5_header(std::vector<uint8_t>& buf,
                       uint32_t magic,
                       uint32_t descriptor_count,
                       uint64_t total_uncompressed_size,
                       uint8_t max_chunk_shift) {
    test::write_u32_le(buf, 0, magic);
    test::write_u32_le(buf, 4, descriptor_count);
    const uint64_t size_and_shift =
        (total_uncompressed_size & kRds5MaxTotalUncompressedSize) |
        (static_cast<uint64_t>(max_chunk_shift) << 56u);
    test::write_u64_le(buf, 8, size_and_shift);
}

uint64_t must_pack(uint64_t disk_offset,
                   uint8_t codec_id,
                   uint64_t virtual_offset,
                   uint16_t trailing_padding) {
    const auto packed = pack_rds5_descriptor(disk_offset, codec_id, virtual_offset, trailing_padding);
    OVF_EXPECT(packed.has_value());
    return packed.value_or(0ull);
}

void write_etpf_header(std::vector<uint8_t>& buf,
                       std::size_t offset,
                       uint32_t magic,
                       uint32_t version,
                       uint32_t entry_count,
                       uint32_t secondary_offset) {
    test::write_u32_le(buf, offset + 0, magic);
    test::write_u32_le(buf, offset + 4, version);
    test::write_u32_le(buf, offset + 8, entry_count);
    test::write_u32_le(buf, offset + 12, secondary_offset);
}

}  // namespace

int main() {
    using namespace open_vfs;
    using namespace open_vfs::rds5;

    // 1. Magic validation for verified LE "RDS5" (0x35534452)
    OVF_EXPECT(validate_rds5_magic(0x35534452u));
    OVF_EXPECT(!validate_rds5_magic(0x52445335u));  // Reversed "5SDR" rejected
    OVF_EXPECT(!validate_rds5_magic(0x00000000u));
    OVF_EXPECT(!validate_rds5_magic(0xDEADBEEFu));

    // 2. Real 16-byte RDS5 header parsing & validation
    {
        std::vector<uint8_t> header_buf(kRds5HeaderSize, 0);
        // Write ASCII bytes 'R','D','S','5' explicitly to confirm LE endianness
        header_buf[0] = 'R';
        header_buf[1] = 'D';
        header_buf[2] = 'S';
        header_buf[3] = '5';
        test::write_u32_le(header_buf, 4, 5u);
        // Exercise high bits of the 56-bit LE total_uncompressed_size + shift 0x16 at +0x0F
        constexpr uint64_t kLargeAlignedVirtualSize = 0x00123456789AB000ull;
        const uint64_t size_and_shift =
            kLargeAlignedVirtualSize | (static_cast<uint64_t>(kRds5MaxChunkShift) << 56u);
        test::write_u64_le(header_buf, 8, size_and_shift);

        Rds5Header hdr{};
        OVF_EXPECT(parse_rds5_header(header_buf, hdr) == Status::Ok);
        OVF_EXPECT(hdr.magic == kRds5Magic);
        OVF_EXPECT(hdr.descriptor_count == 5u);
        OVF_EXPECT(hdr.chunk_count() == 4u);
        OVF_EXPECT(hdr.total_uncompressed_size == kLargeAlignedVirtualSize);
        OVF_EXPECT(hdr.max_chunk_shift == 0x16u);
        OVF_EXPECT(hdr.max_chunk_size() == (4ull * 1024ull * 1024ull));

        // Truncated header rejected and does not clobber existing output
        Rds5Header preserved = hdr;
        OVF_EXPECT(parse_rds5_header({}, preserved) == Status::OutOfBounds);
        OVF_EXPECT(parse_rds5_header(std::span<const uint8_t>(header_buf.data(), 15), preserved) ==
                   Status::OutOfBounds);
        OVF_EXPECT(preserved.descriptor_count == 5u);

        // Bad magic (reversed and invalid)
        auto bad_magic = header_buf;
        test::write_u32_le(bad_magic, 0, 0x52445335u);
        OVF_EXPECT(parse_rds5_header(bad_magic, preserved) == Status::InvalidMagic);
        test::write_u32_le(bad_magic, 0, 0x46505445u);
        OVF_EXPECT(parse_rds5_header(bad_magic, preserved) == Status::InvalidMagic);

        // Descriptor count < 2 rejected (0 and 1)
        auto low_count = header_buf;
        test::write_u32_le(low_count, 4, 0u);
        OVF_EXPECT(parse_rds5_header(low_count, preserved) == Status::CorruptData);
        test::write_u32_le(low_count, 4, 1u);
        OVF_EXPECT(parse_rds5_header(low_count, preserved) == Status::CorruptData);

        // Unsupported max_chunk_shift rejected
        for (uint8_t bad_shift : {uint8_t{0x00u}, uint8_t{0x15u}, uint8_t{0x17u}, uint8_t{0xFFu}}) {
            auto bad_shift_buf = header_buf;
            bad_shift_buf[15] = bad_shift;
            OVF_EXPECT(parse_rds5_header(bad_shift_buf, preserved) == Status::UnsupportedFormat);
        }

        // Non-4KiB-aligned total_uncompressed_size rejected
        for (uint64_t bad_size : {1ull, 256ull, 4095ull, 4097ull}) {
            std::vector<uint8_t> unaligned_buf(kRds5HeaderSize, 0);
            write_rds5_header(unaligned_buf, kRds5Magic, 2u, bad_size, kRds5MaxChunkShift);
            OVF_EXPECT(parse_rds5_header(unaligned_buf, preserved) == Status::AlignmentError);
        }
        OVF_EXPECT(preserved.total_uncompressed_size == kLargeAlignedVirtualSize);
    }

    // 3. 8-byte bitfield packing and unpacking
    {
        // Isolated field bit-position checks
        std::vector<uint8_t> desc_buf(kRds5DescriptorSize, 0);
        Rds5ChunkDescriptor decoded{};

        // disk_offset only: bits [0..25] << 8
        const uint64_t raw_disk = must_pack(0x00012300ull, 0u, 0ull, 0u);
        OVF_EXPECT(raw_disk == 0x00000123ull);
        test::write_u64_le(desc_buf, 0, raw_disk);
        OVF_EXPECT(parse_rds5_descriptor(desc_buf, decoded) == Status::Ok);
        OVF_EXPECT(decoded.disk_offset == 0x00012300ull);
        OVF_EXPECT(decoded.codec_id == 0u);
        OVF_EXPECT(decoded.virtual_offset == 0ull);
        OVF_EXPECT(decoded.trailing_padding == 0u);

        // codec_id only: bits [26..29]
        const uint64_t raw_codec = must_pack(0ull, kRds5CodecZlib, 0ull, 0u);
        OVF_EXPECT(raw_codec == (uint64_t{0x8} << 26u));
        test::write_u64_le(desc_buf, 0, raw_codec);
        OVF_EXPECT(parse_rds5_descriptor(desc_buf, decoded) == Status::Ok);
        OVF_EXPECT(decoded.disk_offset == 0ull);
        OVF_EXPECT(decoded.codec_id == kRds5CodecZlib);
        OVF_EXPECT(decoded.virtual_offset == 0ull);
        OVF_EXPECT(decoded.trailing_padding == 0u);

        // virtual_offset only: bits [30..51] << 12
        const uint64_t raw_virt = must_pack(0ull, 0u, 0x00ABC000ull, 0u);
        OVF_EXPECT(raw_virt == (uint64_t{0xABC} << 30u));
        test::write_u64_le(desc_buf, 0, raw_virt);
        OVF_EXPECT(parse_rds5_descriptor(desc_buf, decoded) == Status::Ok);
        OVF_EXPECT(decoded.disk_offset == 0ull);
        OVF_EXPECT(decoded.codec_id == 0u);
        OVF_EXPECT(decoded.virtual_offset == 0x00ABC000ull);
        OVF_EXPECT(decoded.trailing_padding == 0u);

        // trailing_padding only: bits [52..63]
        const uint64_t raw_pad = must_pack(0ull, 0u, 0ull, 0x0ABCu);
        OVF_EXPECT(raw_pad == (uint64_t{0xABC} << 52u));
        test::write_u64_le(desc_buf, 0, raw_pad);
        OVF_EXPECT(parse_rds5_descriptor(desc_buf, decoded) == Status::Ok);
        OVF_EXPECT(decoded.disk_offset == 0ull);
        OVF_EXPECT(decoded.codec_id == 0u);
        OVF_EXPECT(decoded.virtual_offset == 0ull);
        OVF_EXPECT(decoded.trailing_padding == 0x0ABCu);

        // Maximum values across all 64 bits round-trip cleanly to 0xFFFFFFFFFFFFFFFF
        const uint64_t raw_max = must_pack(
            kRds5MaxDiskOffset, 0x0Fu, kRds5MaxVirtualOffset, kRds5MaxTrailingPadding);
        OVF_EXPECT(raw_max == 0xFFFFFFFFFFFFFFFFull);
        test::write_u64_le(desc_buf, 0, raw_max);
        OVF_EXPECT(parse_rds5_descriptor(desc_buf, decoded) == Status::Ok);
        OVF_EXPECT(decoded.raw == 0xFFFFFFFFFFFFFFFFull);
        OVF_EXPECT(decoded.disk_offset == kRds5MaxDiskOffset);
        OVF_EXPECT(decoded.codec_id == 0x0Fu);
        OVF_EXPECT(decoded.virtual_offset == kRds5MaxVirtualOffset);
        OVF_EXPECT(decoded.trailing_padding == kRds5MaxTrailingPadding);

        // Packer rejects out-of-range or misaligned inputs
        OVF_EXPECT(!pack_rds5_descriptor(0x101ull, 0u, 0ull, 0u).has_value());
        OVF_EXPECT(!pack_rds5_descriptor(kRds5MaxDiskOffset + 0x100ull, 0u, 0ull, 0u).has_value());
        OVF_EXPECT(!pack_rds5_descriptor(0ull, 0x10u, 0ull, 0u).has_value());
        OVF_EXPECT(!pack_rds5_descriptor(0ull, 0u, 0x1001ull, 0u).has_value());
        OVF_EXPECT(!pack_rds5_descriptor(0ull, 0u, kRds5MaxVirtualOffset + 0x1000ull, 0u).has_value());
        OVF_EXPECT(!pack_rds5_descriptor(0ull, 0u, 0ull, 0x1000u).has_value());

        // Truncated descriptor buffer rejected
        OVF_EXPECT(parse_rds5_descriptor({}, decoded) == Status::OutOfBounds);
        OVF_EXPECT(parse_rds5_descriptor(std::span<const uint8_t>(desc_buf.data(), 7), decoded) ==
                   Status::OutOfBounds);
    }

    // 4. Full synthetic multi-chunk RDS5 container + Chunk 0 ETPF resource table
    {
        // 3 chunks + 1 EOF sentinel = 4 descriptors
        // Table occupies [0x10..0x30), zero-padded to 0x100 where Chunk 0 begins.
        // Chunk 0: disk [0x0100..0x0300) span=512, pad=160 -> stored=352, virt [0x000000..0x001000) (4 KiB), raw ETPF
        // Chunk 1: disk [0x0300..0x1400) span=4352, pad=4095 -> stored=257, virt [0x001000..0x401000) (exact 4 MiB), zlib
        // Chunk 2: disk [0x1400..0x1500) span=256, pad=256 -> stored=0, virt [0x401000..0x403000) (8 KiB), codec 0x3
        // Sentinel: disk 0x1500 (file size), virt 0x403000 (total_uncompressed_size)
        constexpr uint32_t kDescCount = 4u;
        constexpr uint64_t kTotalUncompressed = 0x403000ull;
        constexpr std::size_t kContainerSize = 0x1500u;

        std::vector<uint8_t> container(kContainerSize, 0);
        write_rds5_header(container, kRds5Magic, kDescCount, kTotalUncompressed, kRds5MaxChunkShift);

        test::write_u64_le(container, 0x10 + 0 * 8, must_pack(0x0100ull, kRds5CodecRaw, 0x000000ull, 160u));
        test::write_u64_le(container, 0x10 + 1 * 8, must_pack(0x0300ull, kRds5CodecZlib, 0x001000ull, 4095u));
        test::write_u64_le(container, 0x10 + 2 * 8, must_pack(0x1400ull, 0x3u, 0x401000ull, 256u));
        test::write_u64_le(container, 0x10 + 3 * 8, must_pack(kContainerSize, 0x0u, kTotalUncompressed, 0u));

        // Populate unencrypted ETPF resource table at Chunk 0 (offset 0x100, stored_size 352)
        // 12 primary entries => primary_table_end = 0x10 + 12 * 16 = 0xD0 (208 bytes)
        write_etpf_header(container, 0x0100, kEtpfMagic, kEtpfVersion, 12u, 0xD0u);

        Rds5Header hdr{};
        OVF_EXPECT(parse_rds5_header(container, hdr) == Status::Ok);
        OVF_EXPECT(hdr.descriptor_count == kDescCount);
        OVF_EXPECT(hdr.chunk_count() == 3u);
        OVF_EXPECT(hdr.total_uncompressed_size == kTotalUncompressed);

        std::vector<Rds5ChunkDescriptor> descriptors;
        OVF_EXPECT(parse_rds5_descriptor_table(container, hdr, descriptors) == Status::Ok);
        OVF_EXPECT(descriptors.size() == kDescCount);
        OVF_EXPECT(descriptors[0].disk_offset == 0x0100ull);
        OVF_EXPECT(descriptors[0].codec_id == kRds5CodecRaw);
        OVF_EXPECT(descriptors[0].virtual_offset == 0ull);
        OVF_EXPECT(descriptors[0].trailing_padding == 160u);
        OVF_EXPECT(descriptors[3].disk_offset == kContainerSize);
        OVF_EXPECT(descriptors[3].virtual_offset == kTotalUncompressed);

        std::vector<Rds5ChunkInfo> chunks;
        OVF_EXPECT(parse_rds5_chunk_index(container, hdr, chunks) == Status::Ok);
        OVF_EXPECT(chunks.size() == 3u);

        OVF_EXPECT(chunks[0].disk_offset == 0x0100ull);
        OVF_EXPECT(chunks[0].disk_span == 0x0200ull);
        OVF_EXPECT(chunks[0].stored_size == 352ull);
        OVF_EXPECT(chunks[0].virtual_offset == 0ull);
        OVF_EXPECT(chunks[0].uncompressed_size == 0x1000ull);
        OVF_EXPECT(chunks[0].trailing_padding == 160u);
        OVF_EXPECT(chunks[0].codec_id == kRds5CodecRaw);

        OVF_EXPECT(chunks[1].disk_offset == 0x0300ull);
        OVF_EXPECT(chunks[1].disk_span == 0x1100ull);
        OVF_EXPECT(chunks[1].stored_size == 257ull);
        OVF_EXPECT(chunks[1].virtual_offset == 0x1000ull);
        OVF_EXPECT(chunks[1].uncompressed_size == kRds5MaxChunkSize);
        OVF_EXPECT(chunks[1].trailing_padding == 4095u);
        OVF_EXPECT(chunks[1].codec_id == kRds5CodecZlib);

        OVF_EXPECT(chunks[2].disk_offset == 0x1400ull);
        OVF_EXPECT(chunks[2].disk_span == 0x0100ull);
        OVF_EXPECT(chunks[2].stored_size == 0ull);
        OVF_EXPECT(chunks[2].virtual_offset == 0x401000ull);
        OVF_EXPECT(chunks[2].uncompressed_size == 0x2000ull);
        OVF_EXPECT(chunks[2].trailing_padding == 256u);
        OVF_EXPECT(chunks[2].codec_id == 0x3u);

        // Parse ETPF header directly from Chunk 0 stored payload slice
        const std::span<const uint8_t> chunk0_slice(
            container.data() + chunks[0].disk_offset,
            static_cast<std::size_t>(chunks[0].stored_size));
        EtpfHeader etpf{};
        OVF_EXPECT(parse_etpf_header(chunk0_slice, etpf) == Status::Ok);
        OVF_EXPECT(etpf.magic == kEtpfMagic);
        OVF_EXPECT(etpf.version == kEtpfVersion);
        OVF_EXPECT(etpf.entry_count == 12u);
        OVF_EXPECT(etpf.secondary_table_offset == 0xD0u);

        // Prefix-only mode (validate_full_container = false) accepts just header + descriptor table
        const std::span<const uint8_t> table_only(container.data(), 0x10 + kDescCount * 8);
        std::vector<Rds5ChunkInfo> prefix_chunks;
        OVF_EXPECT(parse_rds5_chunk_index(table_only, hdr, prefix_chunks, false) == Status::Ok);
        OVF_EXPECT(prefix_chunks.size() == 3u);
        OVF_EXPECT(prefix_chunks[1].stored_size == 257ull);
        // Same prefix buffer fails full-container validation with OutOfBounds
        OVF_EXPECT(parse_rds5_chunk_index(table_only, hdr, prefix_chunks, true) == Status::OutOfBounds);
        OVF_EXPECT(prefix_chunks.empty());

        // --- Edge & error cases on descriptor table / chunk index ---

        // Stale output is cleared on invalid header fields
        {
            chunks.push_back(Rds5ChunkInfo{});
            Rds5Header bad_hdr = hdr;
            bad_hdr.magic = 0x52445335u;
            OVF_EXPECT(parse_rds5_chunk_index(container, bad_hdr, chunks) == Status::InvalidMagic);
            OVF_EXPECT(chunks.empty());

            chunks.push_back(Rds5ChunkInfo{});
            bad_hdr = hdr;
            bad_hdr.descriptor_count = 1u;
            OVF_EXPECT(parse_rds5_chunk_index(container, bad_hdr, chunks) == Status::CorruptData);
            OVF_EXPECT(chunks.empty());

            chunks.push_back(Rds5ChunkInfo{});
            bad_hdr = hdr;
            bad_hdr.max_chunk_shift = 0x15u;
            OVF_EXPECT(parse_rds5_chunk_index(container, bad_hdr, chunks) == Status::UnsupportedFormat);
            OVF_EXPECT(chunks.empty());

            chunks.push_back(Rds5ChunkInfo{});
            bad_hdr = hdr;
            bad_hdr.total_uncompressed_size = 0x1234ull;
            OVF_EXPECT(parse_rds5_chunk_index(container, bad_hdr, chunks) == Status::AlignmentError);
            OVF_EXPECT(chunks.empty());

            chunks.push_back(Rds5ChunkInfo{});
            bad_hdr = hdr;
            bad_hdr.total_uncompressed_size = 0x0100000000000000ull;  // Exceeds 56-bit header field
            OVF_EXPECT(parse_rds5_chunk_index(container, bad_hdr, chunks) == Status::CorruptData);
            OVF_EXPECT(chunks.empty());
        }

        // Misaligned Chunk 0 disk_offset (0x000 or 0x200 when ALIGN_UP(0x30, 0x100) == 0x100)
        {
            auto misaligned = container;
            test::write_u64_le(misaligned, 0x10, must_pack(0x0000ull, kRds5CodecRaw, 0ull, 0u));
            chunks.push_back(Rds5ChunkInfo{});
            OVF_EXPECT(parse_rds5_chunk_index(misaligned, hdr, chunks) == Status::AlignmentError);
            OVF_EXPECT(chunks.empty());

            test::write_u64_le(misaligned, 0x10, must_pack(0x0200ull, kRds5CodecRaw, 0ull, 0u));
            chunks.push_back(Rds5ChunkInfo{});
            OVF_EXPECT(parse_rds5_chunk_index(misaligned, hdr, chunks) == Status::AlignmentError);
            OVF_EXPECT(chunks.empty());
        }

        // Non-zero Chunk 0 virtual_offset rejected
        {
            auto bad_virt0 = container;
            test::write_u64_le(bad_virt0, 0x10, must_pack(0x0100ull, kRds5CodecRaw, 0x1000ull, 0u));
            OVF_EXPECT(parse_rds5_chunk_index(bad_virt0, hdr, chunks) == Status::CorruptData);
        }

        // Non-monotonic disk_offset rejected
        {
            auto non_mono_disk = container;
            test::write_u64_le(non_mono_disk, 0x10 + 2 * 8, must_pack(0x0200ull, 0x3u, 0x401000ull, 0u));
            OVF_EXPECT(parse_rds5_chunk_index(non_mono_disk, hdr, chunks) == Status::CorruptData);
        }

        // Non-monotonic virtual_offset rejected
        {
            auto non_mono_virt = container;
            test::write_u64_le(non_mono_virt, 0x10 + 1 * 8, must_pack(0x0300ull, kRds5CodecZlib, 0x002000ull, 0u));
            test::write_u64_le(non_mono_virt, 0x10 + 2 * 8, must_pack(0x1400ull, 0x3u, 0x001000ull, 0u));
            OVF_EXPECT(parse_rds5_chunk_index(non_mono_virt, hdr, chunks) == Status::CorruptData);
        }

        // trailing_padding > disk_span rejected (257 > 256)
        {
            auto bad_pad = container;
            test::write_u64_le(bad_pad, 0x10 + 2 * 8, must_pack(0x1400ull, 0x3u, 0x401000ull, 257u));
            OVF_EXPECT(parse_rds5_chunk_index(bad_pad, hdr, chunks) == Status::CorruptData);
        }

        // uncompressed_size > (1ULL << max_chunk_shift) rejected (4 MiB + 4 KiB)
        {
            auto huge_chunk = container;
            constexpr uint64_t kOversizeTotal = 0x404000ull;
            write_rds5_header(huge_chunk, kRds5Magic, kDescCount, kOversizeTotal, kRds5MaxChunkShift);
            test::write_u64_le(huge_chunk, 0x10 + 2 * 8, must_pack(0x1400ull, 0x3u, 0x402000ull, 0u));
            test::write_u64_le(huge_chunk, 0x10 + 3 * 8, must_pack(kContainerSize, 0x0u, kOversizeTotal, 0u));
            Rds5Header oversize_hdr{};
            OVF_EXPECT(parse_rds5_header(huge_chunk, oversize_hdr) == Status::Ok);
            OVF_EXPECT(parse_rds5_chunk_index(huge_chunk, oversize_hdr, chunks) == Status::CorruptData);
        }

        // Sentinel virtual_offset != header.total_uncompressed_size rejected
        {
            auto bad_sentinel_virt = container;
            test::write_u64_le(bad_sentinel_virt, 0x10 + 3 * 8,
                               must_pack(kContainerSize, 0x0u, kTotalUncompressed + 0x1000ull, 0u));
            OVF_EXPECT(parse_rds5_chunk_index(bad_sentinel_virt, hdr, chunks) == Status::CorruptData);
        }

        // Full container truncated by 1 byte -> OutOfBounds
        {
            const std::span<const uint8_t> truncated_eof(container.data(), container.size() - 1u);
            OVF_EXPECT(parse_rds5_chunk_index(truncated_eof, hdr, chunks, true) == Status::OutOfBounds);
        }

        // Extra trailing byte beyond sentinel.disk_offset in full container mode -> CorruptData
        {
            auto padded_eof = container;
            padded_eof.push_back(0x00u);
            OVF_EXPECT(parse_rds5_chunk_index(padded_eof, hdr, chunks, true) == Status::CorruptData);
        }

        // Descriptor table truncated by 1 byte -> OutOfBounds (even in prefix mode)
        {
            const std::span<const uint8_t> truncated_table(container.data(), 0x10 + kDescCount * 8 - 1u);
            OVF_EXPECT(parse_rds5_chunk_index(truncated_table, hdr, chunks, false) == Status::OutOfBounds);
        }

        // 32-bit multiplication overflow trap: 0x20000001u * 8 wraps to 8 in uint32_t
        {
            Rds5Header overflow32_hdr = hdr;
            overflow32_hdr.descriptor_count = 0x20000001u;
            OVF_EXPECT(parse_rds5_descriptor_table(container, overflow32_hdr, descriptors, false) ==
                       Status::OutOfBounds);
            OVF_EXPECT(descriptors.empty());

            Rds5Header max_u32_hdr = hdr;
            max_u32_hdr.descriptor_count = 0xFFFFFFFFu;
            OVF_EXPECT(parse_rds5_descriptor_table(container, max_u32_hdr, descriptors, false) ==
                       Status::OutOfBounds);
            OVF_EXPECT(descriptors.empty());
        }
    }

    // 5. Table alignment boundary: 30 descriptors -> Chunk 0 at 0x100; 31 descriptors -> Chunk 0 at 0x200
    {
        // Exactly 30 descriptors: 0x10 + 30 * 8 = 0x100 => ALIGN_UP(0x100, 0x100) == 0x100
        constexpr uint32_t kCount30 = 30u;
        constexpr uint64_t kTotalVirt30 = (kCount30 - 1u) * 0x1000ull;
        constexpr std::size_t kSize30 = 0x100u + (kCount30 - 1u) * 0x100u;
        std::vector<uint8_t> buf30(kSize30, 0);
        write_rds5_header(buf30, kRds5Magic, kCount30, kTotalVirt30, kRds5MaxChunkShift);
        for (uint32_t i = 0; i < kCount30; ++i) {
            test::write_u64_le(buf30, 0x10 + i * 8,
                               must_pack(0x100ull + i * 0x100ull, kRds5CodecRaw, i * 0x1000ull, 0u));
        }
        Rds5Header hdr30{};
        std::vector<Rds5ChunkInfo> chunks30;
        OVF_EXPECT(parse_rds5_header(buf30, hdr30) == Status::Ok);
        OVF_EXPECT(parse_rds5_chunk_index(buf30, hdr30, chunks30) == Status::Ok);
        OVF_EXPECT(chunks30.size() == 29u);
        OVF_EXPECT(chunks30[0].disk_offset == 0x100ull);

        // 31 descriptors: 0x10 + 31 * 8 = 0x108 => ALIGN_UP(0x108, 0x100) == 0x200
        constexpr uint32_t kCount31 = 31u;
        constexpr uint64_t kTotalVirt31 = (kCount31 - 1u) * 0x1000ull;
        constexpr std::size_t kSize31 = 0x200u + (kCount31 - 1u) * 0x100u;
        std::vector<uint8_t> buf31(kSize31, 0);
        write_rds5_header(buf31, kRds5Magic, kCount31, kTotalVirt31, kRds5MaxChunkShift);
        for (uint32_t i = 0; i < kCount31; ++i) {
            test::write_u64_le(buf31, 0x10 + i * 8,
                               must_pack(0x200ull + i * 0x100ull, kRds5CodecRaw, i * 0x1000ull, 0u));
        }
        Rds5Header hdr31{};
        std::vector<Rds5ChunkInfo> chunks31;
        OVF_EXPECT(parse_rds5_header(buf31, hdr31) == Status::Ok);
        OVF_EXPECT(parse_rds5_chunk_index(buf31, hdr31, chunks31) == Status::Ok);
        OVF_EXPECT(chunks31.size() == 30u);
        OVF_EXPECT(chunks31[0].disk_offset == 0x200ull);

        // Placing Chunk 0 at 0x100 for 31 descriptors must fail AlignmentError
        test::write_u64_le(buf31, 0x10, must_pack(0x100ull, kRds5CodecRaw, 0ull, 0u));
        OVF_EXPECT(parse_rds5_chunk_index(buf31, hdr31, chunks31) == Status::AlignmentError);
    }

    // 6. ETPF Chunk 0 header parsing edge & error cases
    {
        // Exact-fit 0-entry ETPF table (16 bytes total, secondary_table_offset == 0x10)
        std::vector<uint8_t> etpf_empty(kEtpfHeaderSize, 0);
        write_etpf_header(etpf_empty, 0, kEtpfMagic, kEtpfVersion, 0u, 0x10u);
        EtpfHeader parsed{};
        OVF_EXPECT(parse_etpf_header(etpf_empty, parsed) == Status::Ok);
        OVF_EXPECT(parsed.magic == kEtpfMagic);
        OVF_EXPECT(parsed.version == kEtpfVersion);
        OVF_EXPECT(parsed.entry_count == 0u);
        OVF_EXPECT(parsed.secondary_table_offset == 0x10u);

        // Valid table with 3 entries (primary ends at 0x40), secondary starts at 0x50, total 0x80
        std::vector<uint8_t> etpf_buf(0x80, 0);
        write_etpf_header(etpf_buf, 0, kEtpfMagic, kEtpfVersion, 3u, 0x50u);
        OVF_EXPECT(parse_etpf_header(etpf_buf, parsed) == Status::Ok);
        OVF_EXPECT(parsed.entry_count == 3u);
        OVF_EXPECT(parsed.secondary_table_offset == 0x50u);

        // Truncated header (< 16 bytes)
        OVF_EXPECT(parse_etpf_header({}, parsed) == Status::OutOfBounds);
        OVF_EXPECT(parse_etpf_header(std::span<const uint8_t>(etpf_buf.data(), 15), parsed) ==
                   Status::OutOfBounds);

        // Bad magic (reversed and invalid)
        auto bad_etpf = etpf_buf;
        test::write_u32_le(bad_etpf, 0, 0x45545046u);  // Reversed "FPTE"
        OVF_EXPECT(parse_etpf_header(bad_etpf, parsed) == Status::InvalidMagic);
        test::write_u32_le(bad_etpf, 0, 0x00000000u);
        OVF_EXPECT(parse_etpf_header(bad_etpf, parsed) == Status::InvalidMagic);

        // Wrong version
        auto bad_ver = etpf_buf;
        test::write_u32_le(bad_ver, 4, 0x00000279u);
        OVF_EXPECT(parse_etpf_header(bad_ver, parsed) == Status::UnsupportedFormat);
        test::write_u32_le(bad_ver, 4, 0x0000027Bu);
        OVF_EXPECT(parse_etpf_header(bad_ver, parsed) == Status::UnsupportedFormat);

        // Primary table exceeds buffer by 1 byte (3 entries need 0x40 bytes; pass 0x3F)
        OVF_EXPECT(parse_etpf_header(std::span<const uint8_t>(etpf_buf.data(), 0x3F), parsed) ==
                   Status::OutOfBounds);

        // Secondary sub-table offset exceeds buffer by 1 byte (0x81 > 0x80)
        auto oob_sec = etpf_buf;
        test::write_u32_le(oob_sec, 12, 0x81u);
        OVF_EXPECT(parse_etpf_header(oob_sec, parsed) == Status::OutOfBounds);

        // Secondary sub-table offset overlaps header (0x00, 0x08) or primary descriptors (0x3F)
        auto overlap_sec = etpf_buf;
        for (uint32_t bad_sec : {0x00u, 0x08u, 0x3Fu}) {
            test::write_u32_le(overlap_sec, 12, bad_sec);
            OVF_EXPECT(parse_etpf_header(overlap_sec, parsed) == Status::CorruptData);
        }

        // 32-bit multiplication overflow trap: 0x10000001u * 16 wraps to 16 in uint32_t
        auto overflow_etpf = etpf_buf;
        test::write_u32_le(overflow_etpf, 8, 0x10000001u);
        OVF_EXPECT(parse_etpf_header(overflow_etpf, parsed) == Status::OutOfBounds);
        test::write_u32_le(overflow_etpf, 8, 0xFFFFFFFFu);
        OVF_EXPECT(parse_etpf_header(overflow_etpf, parsed) == Status::OutOfBounds);
    }

    // 7. Pending stubs return NotImplemented cleanly
    {
        std::vector<uint8_t> raw_chunk;
        std::vector<uint8_t> plain_chunk;
        Rds5ChunkInfo dummy_chunk{};
        OVF_EXPECT(read_rds5_stream_chunk({}, dummy_chunk, raw_chunk) == Status::NotImplemented);
        OVF_EXPECT(decompress_rds5_chunk(dummy_chunk, {}, plain_chunk) == Status::NotImplemented);

        std::vector<EtpfResourceDescriptor> res_descs;
        std::vector<std::size_t> dep_indices;
        OVF_EXPECT(resolve_rds5_dependencies(res_descs, 0ull, dep_indices) == Status::NotImplemented);

        std::vector<uint8_t> image;
        EtpfResourceDescriptor dummy_res{};
        OVF_EXPECT(assemble_rds5_resource(dummy_res, {}, {}, image) == Status::NotImplemented);
    }

    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
