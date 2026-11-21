#include "test_harness.hpp"
#include "rds5/rds5.hpp"

#include <array>
#include <cstdint>
#include <optional>
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
                       uint32_t aux_pool_size) {
    test::write_u32_le(buf, offset + 0, magic);
    test::write_u32_le(buf, offset + 4, version);
    test::write_u32_le(buf, offset + 8, entry_count);
    test::write_u32_le(buf, offset + 12, aux_pool_size);
}

void write_etpf_descriptor(std::vector<uint8_t>& buf,
                           std::size_t offset,
                           const EtpfTextureDescriptor& desc) {
    const auto packed = pack_etpf_texture_descriptor(desc);
    OVF_EXPECT(packed.has_value());
    const auto words = packed.value_or(EtpfPackedDescriptorWords{});
    test::write_u32_le(buf, offset + 0, words.w0);
    test::write_u32_le(buf, offset + 4, words.w1);
    test::write_u64_le(buf, offset + 8, words.q23);
}

EtpfTextureDescriptor make_texture_desc(uint16_t width,
                                        uint16_t height,
                                        uint8_t dimension_flags,
                                        uint8_t depth_or_slices,
                                        uint32_t virt_page,
                                        std::array<uint8_t, 4> chunk_codes = {11u, 0u, 0u, 0u},
                                        uint8_t dxgi_format = 98u) {
    EtpfTextureDescriptor desc{};
    desc.width = width;
    desc.height = height;
    desc.tail_mip_count = 4u;
    desc.streamed_mip_count = 2u;
    desc.dxgi_format = dxgi_format;
    desc.depth_or_slices = depth_or_slices;
    desc.dimension_flags = dimension_flags;
    desc.packed_tier_count = 1u;
    desc.is_cube_tile_flag = (dimension_flags == static_cast<uint8_t>(EtpfTextureDimension::TextureCube));
    desc.aux_flag_10 = false;
    desc.virt_page = virt_page;
    desc.chunk_size_codes = chunk_codes;
    desc.sub64k_chunk_count = 1u;
    desc.tile0_free_pages = 3u;
    return desc;
}

std::vector<uint8_t> build_synthetic_etpf_chunk(
    const std::vector<EtpfTextureDescriptor>& descriptors,
    const std::vector<uint64_t>& keys,
    const std::vector<uint64_t>& secondary_entries,
    const std::vector<uint8_t>& aux_pool,
    std::size_t padded_size = 0u) {
    OVF_EXPECT(descriptors.size() == keys.size());
    OVF_EXPECT(descriptors.size() == secondary_entries.size());

    const std::size_t n = descriptors.size();
    const std::size_t active_span = kEtpfHeaderSize + (n * kEtpfTotalBytesPerEntry) + aux_pool.size();
    const std::size_t total_size = padded_size > active_span ? padded_size : active_span;
    std::vector<uint8_t> buf(total_size, 0);

    write_etpf_header(buf, 0, kEtpfMagic, kEtpfVersion,
                      static_cast<uint32_t>(n),
                      static_cast<uint32_t>(aux_pool.size()));

    std::size_t cursor = kEtpfHeaderSize;
    for (const auto& desc : descriptors) {
        write_etpf_descriptor(buf, cursor, desc);
        cursor += kEtpfDescriptorSize;
    }
    for (uint64_t key : keys) {
        test::write_u64_le(buf, cursor, key);
        cursor += kEtpfKeySize;
    }
    for (uint64_t sec : secondary_entries) {
        test::write_u64_le(buf, cursor, sec);
        cursor += kEtpfSecondaryEntrySize;
    }
    for (std::size_t i = 0; i < aux_pool.size(); ++i) {
        buf[cursor + i] = aux_pool[i];
    }
    return buf;
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
        // 10 entries + 16-byte aux pool => 0x10 + 32 * 10 + 16 = 352 bytes exact fit
        {
            std::vector<EtpfTextureDescriptor> chunk0_descs;
            std::vector<uint64_t> chunk0_keys;
            std::vector<uint64_t> chunk0_sec;
            for (uint32_t i = 0; i < 10u; ++i) {
                chunk0_descs.push_back(make_texture_desc(
                    static_cast<uint16_t>(64u << (i % 4u)),
                    static_cast<uint16_t>(64u << (i % 4u)),
                    static_cast<uint8_t>(EtpfTextureDimension::Texture2D),
                    1u,
                    i * 16u,
                    {11u, 0u, 0u, 0u}));
                chunk0_keys.push_back((uint64_t{i + 1u} << 4u) | 0x3ull);
                chunk0_sec.push_back(0xA5A5000000000000ull ^ static_cast<uint64_t>(i));
            }
            const std::vector<uint8_t> chunk0_aux(16u, 0x5Au);
            const auto chunk0_bytes =
                build_synthetic_etpf_chunk(chunk0_descs, chunk0_keys, chunk0_sec, chunk0_aux);
            OVF_EXPECT(chunk0_bytes.size() == 352u);
            for (std::size_t i = 0; i < chunk0_bytes.size(); ++i) {
                container[0x0100u + i] = chunk0_bytes[i];
            }
        }

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

        // Parse ETPF header and full table directly from Chunk 0 stored payload slice
        const std::span<const uint8_t> chunk0_slice(
            container.data() + chunks[0].disk_offset,
            static_cast<std::size_t>(chunks[0].stored_size));
        EtpfHeader etpf{};
        OVF_EXPECT(parse_etpf_header(chunk0_slice, etpf) == Status::Ok);
        OVF_EXPECT(etpf.magic == kEtpfMagic);
        OVF_EXPECT(etpf.version == kEtpfVersion);
        OVF_EXPECT(etpf.entry_count == 10u);
        OVF_EXPECT(etpf.aux_pool_size == 16u);
        OVF_EXPECT(etpf.total_span() == 352ull);

        EtpfTable chunk0_table{};
        OVF_EXPECT(parse_etpf_table(chunk0_slice, chunk0_table) == Status::Ok);
        OVF_EXPECT(chunk0_table.descriptors.size() == 10u);
        OVF_EXPECT(chunk0_table.resource_keys.size() == 10u);
        OVF_EXPECT(chunk0_table.secondary_entries.size() == 10u);
        OVF_EXPECT(chunk0_table.aux_pool.size() == 16u);
        OVF_EXPECT(chunk0_table.find_key_index((uint64_t{5u} << 4u) | 0x3ull) == std::optional<std::size_t>{4u});

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

    // 6. ETPF Chunk 0 header parsing (0x10 + 32*N + S_aux), odd/even N, alignment & overflow traps
    {
        // Exact-fit 0-entry ETPF table (16 bytes total, aux_pool_size == 0)
        std::vector<uint8_t> etpf_empty(kEtpfHeaderSize, 0);
        write_etpf_header(etpf_empty, 0, kEtpfMagic, kEtpfVersion, 0u, 0u);
        EtpfHeader parsed{};
        OVF_EXPECT(parse_etpf_header(etpf_empty, parsed) == Status::Ok);
        OVF_EXPECT(parsed.magic == kEtpfMagic);
        OVF_EXPECT(parsed.version == kEtpfVersion);
        OVF_EXPECT(parsed.entry_count == 0u);
        OVF_EXPECT(parsed.aux_pool_size == 0u);
        OVF_EXPECT(parsed.descriptors_offset() == 0x10ull);
        OVF_EXPECT(parsed.keys_offset() == 0x10ull);
        OVF_EXPECT(parsed.secondary_offset() == 0x10ull);
        OVF_EXPECT(parsed.aux_pool_offset() == 0x10ull);
        OVF_EXPECT(parsed.total_span() == 0x10ull);

        // 0 entries with 32-byte aligned aux pool -> exact fit 48 bytes (0x30)
        std::vector<uint8_t> etpf_aux_only(0x30u, 0);
        write_etpf_header(etpf_aux_only, 0, kEtpfMagic, kEtpfVersion, 0u, 32u);
        OVF_EXPECT(parse_etpf_header(etpf_aux_only, parsed) == Status::Ok);
        OVF_EXPECT(parsed.entry_count == 0u);
        OVF_EXPECT(parsed.aux_pool_size == 32u);
        OVF_EXPECT(parsed.total_span() == 0x30ull);
        OVF_EXPECT(parse_etpf_header(std::span<const uint8_t>(etpf_aux_only.data(), 0x2Fu), parsed) ==
                   Status::OutOfBounds);

        // Odd entry_count (N = 3, S_aux = 0):
        // descriptors [0x10..0x40), keys [0x40..0x58) (8 mod 16), secondary [0x58..0x70), aux [0x70..0x70)
        std::vector<uint8_t> etpf_odd(0x70u, 0);
        write_etpf_header(etpf_odd, 0, kEtpfMagic, kEtpfVersion, 3u, 0u);
        OVF_EXPECT(parse_etpf_header(etpf_odd, parsed) == Status::Ok);
        OVF_EXPECT(parsed.entry_count == 3u);
        OVF_EXPECT(parsed.aux_pool_size == 0u);
        OVF_EXPECT(parsed.keys_offset() == 0x40ull);
        OVF_EXPECT(parsed.secondary_offset() == 0x58ull);
        OVF_EXPECT(parsed.aux_pool_offset() == 0x70ull);
        OVF_EXPECT(parsed.total_span() == 0x70ull);
        OVF_EXPECT(parse_etpf_header(std::span<const uint8_t>(etpf_odd.data(), 0x6Fu), parsed) ==
                   Status::OutOfBounds);

        // Odd entry_count with aux pool (N = 5, S_aux = 32) inside 4 KiB Chunk 0 zero-padded buffer
        // 0x10 + 32 * 5 + 32 = 0xD0 (208 bytes)
        std::vector<uint8_t> etpf_odd_padded(4096u, 0);
        write_etpf_header(etpf_odd_padded, 0, kEtpfMagic, kEtpfVersion, 5u, 32u);
        OVF_EXPECT(parse_etpf_header(etpf_odd_padded, parsed) == Status::Ok);
        OVF_EXPECT(parsed.entry_count == 5u);
        OVF_EXPECT(parsed.aux_pool_size == 32u);
        OVF_EXPECT(parsed.keys_offset() == 0x60ull);
        OVF_EXPECT(parsed.secondary_offset() == 0x88ull);
        OVF_EXPECT(parsed.aux_pool_offset() == 0xB0ull);
        OVF_EXPECT(parsed.total_span() == 0xD0ull);
        OVF_EXPECT(parse_etpf_header(std::span<const uint8_t>(etpf_odd_padded.data(), 0xD0u), parsed) ==
                   Status::Ok);
        OVF_EXPECT(parse_etpf_header(std::span<const uint8_t>(etpf_odd_padded.data(), 0xCFu), parsed) ==
                   Status::OutOfBounds);

        // Even entry_count (N = 4, S_aux = 16): 0x10 + 32 * 4 + 16 = 0xA0 (160 bytes)
        std::vector<uint8_t> etpf_even(0xA0u, 0);
        write_etpf_header(etpf_even, 0, kEtpfMagic, kEtpfVersion, 4u, 16u);
        OVF_EXPECT(parse_etpf_header(etpf_even, parsed) == Status::Ok);
        OVF_EXPECT(parsed.entry_count == 4u);
        OVF_EXPECT(parsed.aux_pool_size == 16u);
        OVF_EXPECT(parsed.keys_offset() == 0x50ull);
        OVF_EXPECT(parsed.secondary_offset() == 0x70ull);
        OVF_EXPECT(parsed.aux_pool_offset() == 0x90ull);
        OVF_EXPECT(parsed.total_span() == 0xA0ull);
        OVF_EXPECT(parse_etpf_header(std::span<const uint8_t>(etpf_even.data(), 0x9Fu), parsed) ==
                   Status::OutOfBounds);

        // Truncated header (< 16 bytes) preserves previous output
        EtpfHeader preserved = parsed;
        OVF_EXPECT(parse_etpf_header({}, preserved) == Status::OutOfBounds);
        OVF_EXPECT(parse_etpf_header(std::span<const uint8_t>(etpf_even.data(), 15), preserved) ==
                   Status::OutOfBounds);
        OVF_EXPECT(preserved.entry_count == 4u);
        OVF_EXPECT(preserved.aux_pool_size == 16u);

        // Bad magic (reversed and invalid)
        auto bad_etpf = etpf_even;
        test::write_u32_le(bad_etpf, 0, 0x45545046u);  // Reversed "FPTE"
        OVF_EXPECT(parse_etpf_header(bad_etpf, preserved) == Status::InvalidMagic);
        test::write_u32_le(bad_etpf, 0, 0x00000000u);
        OVF_EXPECT(parse_etpf_header(bad_etpf, preserved) == Status::InvalidMagic);

        // Wrong version
        auto bad_ver = etpf_even;
        test::write_u32_le(bad_ver, 4, 0x00000279u);
        OVF_EXPECT(parse_etpf_header(bad_ver, preserved) == Status::UnsupportedFormat);
        test::write_u32_le(bad_ver, 4, 0x0000027Bu);
        OVF_EXPECT(parse_etpf_header(bad_ver, preserved) == Status::UnsupportedFormat);

        // Misaligned aux_pool_size (S_aux % 16 != 0) rejected with AlignmentError
        auto bad_aux_align = etpf_even;
        for (uint32_t bad_aux : {1u, 4u, 8u, 12u, 15u, 17u, 24u, 31u, 0xFFFFFFFFu}) {
            test::write_u32_le(bad_aux_align, 12, bad_aux);
            OVF_EXPECT(parse_etpf_header(bad_aux_align, preserved) == Status::AlignmentError);
        }

        // Aligned aux_pool_size exceeding buffer by 16 bytes -> OutOfBounds
        auto oob_aux = etpf_even;
        test::write_u32_le(oob_aux, 12, 32u);
        OVF_EXPECT(parse_etpf_header(oob_aux, preserved) == Status::OutOfBounds);

        // 32-bit multiplication & addition overflow traps
        auto overflow_etpf = etpf_even;
        // 0x08000001u * 32 wraps to 32 in uint32_t
        test::write_u32_le(overflow_etpf, 8, 0x08000001u);
        test::write_u32_le(overflow_etpf, 12, 0u);
        OVF_EXPECT(parse_etpf_header(overflow_etpf, preserved) == Status::OutOfBounds);
        // 0x10000001u * 16 wraps to 16 in uint32_t
        test::write_u32_le(overflow_etpf, 8, 0x10000001u);
        OVF_EXPECT(parse_etpf_header(overflow_etpf, preserved) == Status::OutOfBounds);
        // Max u32 entry_count with zero or max aligned aux_pool_size
        test::write_u32_le(overflow_etpf, 8, 0xFFFFFFFFu);
        test::write_u32_le(overflow_etpf, 12, 0u);
        OVF_EXPECT(parse_etpf_header(overflow_etpf, preserved) == Status::OutOfBounds);
        test::write_u32_le(overflow_etpf, 12, 0xFFFFFFF0u);
        OVF_EXPECT(parse_etpf_header(overflow_etpf, preserved) == Status::OutOfBounds);
        // Zero entries with huge aligned aux_pool_size
        test::write_u32_le(overflow_etpf, 8, 0u);
        test::write_u32_le(overflow_etpf, 12, 0xFFFFFFF0u);
        OVF_EXPECT(parse_etpf_header(overflow_etpf, preserved) == Status::OutOfBounds);
        OVF_EXPECT(preserved.entry_count == 4u);
    }

    // 7. Verified 5-bit ETPF chunk_size_codes lookup table (exhaustive codes 0..31 + out-of-range)
    {
        constexpr std::array<std::optional<uint32_t>, 32> kExpectedPages = {{
            0u,            //  0: absent / empty slot
            1u,            //  1:    4 KiB
            2u,            //  2:    8 KiB
            3u,            //  3:   12 KiB
            4u,            //  4:   16 KiB
            5u,            //  5:   20 KiB
            6u,            //  6:   24 KiB
            7u,            //  7:   28 KiB
            8u,            //  8:   32 KiB
            std::nullopt,  //  9: unobserved
            12u,           // 10:   48 KiB
            16u,           // 11:   64 KiB
            24u,           // 12:   96 KiB
            32u,           // 13:  128 KiB
            48u,           // 14:  192 KiB
            64u,           // 15:  256 KiB
            128u,          // 16:  512 KiB
            std::nullopt,  // 17: unobserved
            std::nullopt,  // 18: unobserved
            std::nullopt,  // 19: unobserved
            std::nullopt,  // 20: unobserved
            18u,           // 21:   72 KiB
            80u,           // 22:  320 KiB
            96u,           // 23:  384 KiB
            144u,          // 24:  576 KiB
            std::nullopt,  // 25: unobserved
            272u,          // 26: 1088 KiB
            384u,          // 27: 1536 KiB
            528u,          // 28: 2112 KiB
            std::nullopt,  // 29: unobserved
            std::nullopt,  // 30: unobserved
            std::nullopt,  // 31: unobserved
        }};

        for (uint8_t code = 0u; code < 32u; ++code) {
            const auto pages = lookup_etpf_chunk_size_pages(code);
            const auto bytes = lookup_etpf_chunk_size_bytes(code);
            OVF_EXPECT(pages == kExpectedPages[code]);
            if (kExpectedPages[code].has_value()) {
                OVF_EXPECT(bytes.has_value());
                OVF_EXPECT(*bytes == static_cast<uint64_t>(*kExpectedPages[code]) * kRds5VirtualPageSize);
            } else {
                OVF_EXPECT(!bytes.has_value());
            }
        }

        // Out-of-range (> 31) codes return std::nullopt
        for (uint8_t bad_code : {uint8_t{32u}, uint8_t{33u}, uint8_t{63u}, uint8_t{255u}}) {
            OVF_EXPECT(!lookup_etpf_chunk_size_pages(bad_code).has_value());
            OVF_EXPECT(!lookup_etpf_chunk_size_bytes(bad_code).has_value());
        }
    }

    // 8. ETPF 16-byte GPU texture descriptor bitfield round-trips (2D, Cube, 3D, 2DArray) & errors
    {
        auto verify_round_trip = [](const EtpfTextureDescriptor& expected) {
            std::vector<uint8_t> buf(kEtpfDescriptorSize, 0);
            write_etpf_descriptor(buf, 0, expected);

            EtpfTextureDescriptor decoded{};
            OVF_EXPECT(parse_etpf_texture_descriptor(buf, decoded) == Status::Ok);
            OVF_EXPECT(decoded.width == expected.width);
            OVF_EXPECT(decoded.height == expected.height);
            OVF_EXPECT(decoded.tail_mip_count == expected.tail_mip_count);
            OVF_EXPECT(decoded.streamed_mip_count == expected.streamed_mip_count);
            OVF_EXPECT(decoded.dxgi_format == expected.dxgi_format);
            OVF_EXPECT(decoded.depth_or_slices == expected.depth_or_slices);
            OVF_EXPECT(decoded.dimension_flags == expected.dimension_flags);
            OVF_EXPECT(decoded.dimension() == static_cast<EtpfTextureDimension>(expected.dimension_flags));
            OVF_EXPECT(decoded.packed_tier_count == expected.packed_tier_count);
            OVF_EXPECT(decoded.is_cube_tile_flag == expected.is_cube_tile_flag);
            OVF_EXPECT(decoded.aux_flag_10 == expected.aux_flag_10);
            OVF_EXPECT(decoded.virt_page == expected.virt_page);
            OVF_EXPECT(decoded.virtual_offset_bytes() ==
                       static_cast<uint64_t>(expected.virt_page) * kRds5VirtualPageSize);
            OVF_EXPECT(decoded.chunk_size_codes == expected.chunk_size_codes);
            OVF_EXPECT(decoded.sub64k_chunk_count == expected.sub64k_chunk_count);
            OVF_EXPECT(decoded.tile0_free_pages == expected.tile0_free_pages);
        };

        // Texture2D (dimension_flags = 1)
        {
            EtpfTextureDescriptor d2d{};
            d2d.width = 2048u;
            d2d.height = 1024u;
            d2d.tail_mip_count = 6u;
            d2d.streamed_mip_count = 3u;
            d2d.dxgi_format = 98u;  // BC7_UNORM
            d2d.depth_or_slices = 1u;
            d2d.dimension_flags = static_cast<uint8_t>(EtpfTextureDimension::Texture2D);
            d2d.packed_tier_count = 2u;
            d2d.is_cube_tile_flag = false;
            d2d.aux_flag_10 = false;
            d2d.virt_page = 0x001234u;
            d2d.chunk_size_codes = {15u, 13u, 11u, 4u};
            d2d.sub64k_chunk_count = 1u;
            d2d.tile0_free_pages = 7u;
            verify_round_trip(d2d);
        }

        // TextureCube (dimension_flags = 2)
        {
            EtpfTextureDescriptor cube{};
            cube.width = 512u;
            cube.height = 512u;
            cube.tail_mip_count = 5u;
            cube.streamed_mip_count = 2u;
            cube.dxgi_format = 95u;  // BC6H_UF16
            cube.depth_or_slices = 6u;
            cube.dimension_flags = static_cast<uint8_t>(EtpfTextureDimension::TextureCube);
            cube.packed_tier_count = 4u;
            cube.is_cube_tile_flag = true;
            cube.aux_flag_10 = true;
            cube.virt_page = 0x045678u;
            cube.chunk_size_codes = {23u, 14u, 10u, 0u};
            cube.sub64k_chunk_count = 2u;
            cube.tile0_free_pages = 11u;
            verify_round_trip(cube);
        }

        // Texture3D (dimension_flags = 3)
        {
            EtpfTextureDescriptor vol{};
            vol.width = 64u;
            vol.height = 128u;
            vol.tail_mip_count = 3u;
            vol.streamed_mip_count = 1u;
            vol.dxgi_format = 71u;  // BC1_UNORM
            vol.depth_or_slices = 32u;
            vol.dimension_flags = static_cast<uint8_t>(EtpfTextureDimension::Texture3D);
            vol.packed_tier_count = 3u;
            vol.is_cube_tile_flag = false;
            vol.aux_flag_10 = true;
            vol.virt_page = 0x0ABCDEu;
            vol.chunk_size_codes = {21u, 12u, 8u, 1u};
            vol.sub64k_chunk_count = 3u;
            vol.tile0_free_pages = 0u;
            verify_round_trip(vol);
        }

        // Texture2DArray (dimension_flags = 5)
        {
            EtpfTextureDescriptor arr{};
            arr.width = 1024u;
            arr.height = 1024u;
            arr.tail_mip_count = 9u;
            arr.streamed_mip_count = 4u;
            arr.dxgi_format = 83u;  // BC5_UNORM
            arr.depth_or_slices = 16u;
            arr.dimension_flags = static_cast<uint8_t>(EtpfTextureDimension::Texture2DArray);
            arr.packed_tier_count = 5u;
            arr.is_cube_tile_flag = false;
            arr.aux_flag_10 = true;
            arr.virt_page = 0x10F00Du;
            arr.chunk_size_codes = {28u, 27u, 26u, 24u};
            arr.sub64k_chunk_count = 0u;
            arr.tile0_free_pages = 15u;
            verify_round_trip(arr);
        }

        // Min & Max extreme bitfield boundaries (confirming zero bit bleed across all fields)
        {
            EtpfTextureDescriptor min_desc{};
            min_desc.width = 1u;
            min_desc.height = 1u;
            min_desc.tail_mip_count = 0u;
            min_desc.streamed_mip_count = 0u;
            min_desc.dxgi_format = 0u;
            min_desc.depth_or_slices = 1u;
            min_desc.dimension_flags = static_cast<uint8_t>(EtpfTextureDimension::Texture2D);
            min_desc.packed_tier_count = 0u;
            min_desc.is_cube_tile_flag = false;
            min_desc.aux_flag_10 = false;
            min_desc.virt_page = 0u;
            min_desc.chunk_size_codes = {0u, 0u, 0u, 0u};
            min_desc.sub64k_chunk_count = 0u;
            min_desc.tile0_free_pages = 0u;
            verify_round_trip(min_desc);

            EtpfTextureDescriptor max_desc{};
            max_desc.width = kEtpfMaxDimension;
            max_desc.height = kEtpfMaxDimension;
            max_desc.tail_mip_count = 15u;
            max_desc.streamed_mip_count = 7u;
            max_desc.dxgi_format = 127u;
            max_desc.depth_or_slices = kEtpfMaxDepthOrSlices;
            max_desc.dimension_flags = static_cast<uint8_t>(EtpfTextureDimension::Texture2DArray);
            max_desc.packed_tier_count = 7u;
            max_desc.is_cube_tile_flag = true;
            max_desc.aux_flag_10 = true;
            max_desc.virt_page = kEtpfMaxVirtPage;
            max_desc.chunk_size_codes = {31u, 30u, 29u, 25u};  // Unobserved raw codes still decode
            max_desc.sub64k_chunk_count = 3u;
            max_desc.tile0_free_pages = 15u;
            verify_round_trip(max_desc);

            // Verify raw bit positions explicitly on max_desc
            const auto packed_max = pack_etpf_texture_descriptor(max_desc);
            OVF_EXPECT(packed_max.has_value());
            OVF_EXPECT(packed_max->w0 == 0xFFFFFFFFu);
            OVF_EXPECT(packed_max->w1 == ((7u) | (127u << 3u) | (63u << 10u) | (5u << 21u) |
                                          (7u << 24u) | (1u << 27u) | (1u << 28u)));
            OVF_EXPECT(packed_max->q23 == (0x1FFFFFull | (31ull << 21u) | (30ull << 26u) |
                                           (29ull << 31u) | (25ull << 36u) | (3ull << 51u) |
                                           (15ull << 53u)));
        }

        // Strict rejection of invalid dimension_flags outside {1, 2, 3, 5}
        {
            std::vector<uint8_t> raw_desc(kEtpfDescriptorSize, 0);
            EtpfTextureDescriptor preserved = make_texture_desc(
                256u, 256u, static_cast<uint8_t>(EtpfTextureDimension::Texture2D), 1u, 42u);
            for (uint8_t bad_dim : {uint8_t{0u}, uint8_t{4u}, uint8_t{6u}, uint8_t{7u}}) {
                test::write_u32_le(raw_desc, 0, 0u);
                test::write_u32_le(raw_desc, 4, static_cast<uint32_t>(bad_dim) << 21u);
                test::write_u64_le(raw_desc, 8, 0ull);
                OVF_EXPECT(parse_etpf_texture_descriptor(raw_desc, preserved) ==
                           Status::UnsupportedFormat);
                OVF_EXPECT(preserved.virt_page == 42u);
            }

            // Truncated descriptor buffer rejected without clobbering output
            OVF_EXPECT(parse_etpf_texture_descriptor({}, preserved) == Status::OutOfBounds);
            OVF_EXPECT(parse_etpf_texture_descriptor(
                           std::span<const uint8_t>(raw_desc.data(), 15), preserved) ==
                       Status::OutOfBounds);
            OVF_EXPECT(preserved.virt_page == 42u);
        }

        // Packer rejects out-of-range fields
        {
            EtpfTextureDescriptor valid = make_texture_desc(
                128u, 128u, static_cast<uint8_t>(EtpfTextureDimension::Texture2D), 1u, 1u);
            OVF_EXPECT(pack_etpf_texture_descriptor(valid).has_value());

            auto bad = valid; bad.width = 0u;
            OVF_EXPECT(!pack_etpf_texture_descriptor(bad).has_value());
            bad = valid; bad.width = kEtpfMaxDimension + 1u;
            OVF_EXPECT(!pack_etpf_texture_descriptor(bad).has_value());
            bad = valid; bad.height = 0u;
            OVF_EXPECT(!pack_etpf_texture_descriptor(bad).has_value());
            bad = valid; bad.depth_or_slices = 0u;
            OVF_EXPECT(!pack_etpf_texture_descriptor(bad).has_value());
            bad = valid; bad.depth_or_slices = kEtpfMaxDepthOrSlices + 1u;
            OVF_EXPECT(!pack_etpf_texture_descriptor(bad).has_value());
            bad = valid; bad.dimension_flags = 4u;
            OVF_EXPECT(!pack_etpf_texture_descriptor(bad).has_value());
            bad = valid; bad.virt_page = kEtpfMaxVirtPage + 1u;
            OVF_EXPECT(!pack_etpf_texture_descriptor(bad).has_value());
            bad = valid; bad.chunk_size_codes[2] = 32u;
            OVF_EXPECT(!pack_etpf_texture_descriptor(bad).has_value());
            bad = valid; bad.sub64k_chunk_count = 4u;
            OVF_EXPECT(!pack_etpf_texture_descriptor(bad).has_value());
            bad = valid; bad.tile0_free_pages = 16u;
            OVF_EXPECT(!pack_etpf_texture_descriptor(bad).has_value());
        }
    }

    // 9. Full ETPF Chunk 0 table parsing: odd/even N, monotonic virt_page & ascending u64 keys
    {
        // Odd N = 3, S_aux = 32, padded to 4 KiB Chunk 0 boundary; equal adjacent virt_page allowed
        const std::vector<EtpfTextureDescriptor> odd_descs = {
            make_texture_desc(256u, 256u, static_cast<uint8_t>(EtpfTextureDimension::Texture2D), 1u, 10u, {11u, 0u, 0u, 0u}),
            make_texture_desc(128u, 128u, static_cast<uint8_t>(EtpfTextureDimension::TextureCube), 6u, 10u, {14u, 10u, 0u, 0u}),
            make_texture_desc(64u, 64u, static_cast<uint8_t>(EtpfTextureDimension::Texture3D), 16u, 42u, {13u, 8u, 4u, 0u}),
        };
        // Strictly ascending sharded keys (low 3 bits == shard 5)
        const std::vector<uint64_t> odd_keys = {
            0x0000000000001005ull,
            0x0000000100002005ull,
            0xF000000000003005ull,
        };
        const std::vector<uint64_t> odd_sec = {
            0x1122334455667788ull,
            0x99AABBCCDDEEFF00ull,
            0xCAFEBABE12345678ull,
        };
        std::vector<uint8_t> odd_aux(32u);
        for (std::size_t i = 0; i < odd_aux.size(); ++i) {
            odd_aux[i] = static_cast<uint8_t>(0xC0u + i);
        }

        const auto odd_chunk0 = build_synthetic_etpf_chunk(odd_descs, odd_keys, odd_sec, odd_aux, 4096u);
        OVF_EXPECT(odd_chunk0.size() == 4096u);

        EtpfTable table{};
        OVF_EXPECT(parse_etpf_table(odd_chunk0, table) == Status::Ok);
        OVF_EXPECT(table.header.entry_count == 3u);
        OVF_EXPECT(table.header.aux_pool_size == 32u);
        OVF_EXPECT(table.header.keys_offset() == 0x40ull);
        OVF_EXPECT(table.header.secondary_offset() == 0x58ull);
        OVF_EXPECT(table.header.aux_pool_offset() == 0x70ull);
        OVF_EXPECT(table.descriptors.size() == 3u);
        OVF_EXPECT(table.descriptors[0].virt_page == 10u);
        OVF_EXPECT(table.descriptors[1].virt_page == 10u);
        OVF_EXPECT(table.descriptors[1].dimension() == EtpfTextureDimension::TextureCube);
        OVF_EXPECT(table.descriptors[2].virt_page == 42u);
        OVF_EXPECT(table.descriptors[2].dimension() == EtpfTextureDimension::Texture3D);
        OVF_EXPECT(table.resource_keys == odd_keys);
        OVF_EXPECT(table.secondary_entries == odd_sec);
        OVF_EXPECT(table.aux_pool == odd_aux);
        OVF_EXPECT(table.find_key_index(odd_keys[0]) == std::optional<std::size_t>{0u});
        OVF_EXPECT(table.find_key_index(odd_keys[1]) == std::optional<std::size_t>{1u});
        OVF_EXPECT(table.find_key_index(odd_keys[2]) == std::optional<std::size_t>{2u});
        OVF_EXPECT(!table.find_key_index(0x0000000000001006ull).has_value());

        // Even N = 4 exact-fit (no trailing padding), covering all 4 dimension kinds
        const std::vector<EtpfTextureDescriptor> even_descs = {
            make_texture_desc(512u, 256u, static_cast<uint8_t>(EtpfTextureDimension::Texture2D), 1u, 0u, {13u, 0u, 0u, 0u}),
            make_texture_desc(256u, 256u, static_cast<uint8_t>(EtpfTextureDimension::TextureCube), 6u, 32u, {23u, 11u, 0u, 0u}),
            make_texture_desc(32u, 32u, static_cast<uint8_t>(EtpfTextureDimension::Texture3D), 32u, 128u, {11u, 0u, 0u, 0u}),
            make_texture_desc(1024u, 1024u, static_cast<uint8_t>(EtpfTextureDimension::Texture2DArray), 8u, 256u, {28u, 26u, 24u, 16u}),
        };
        const std::vector<uint64_t> even_keys = {10ull, 20ull, 30ull, 40ull};
        const std::vector<uint64_t> even_sec = {1ull, 2ull, 3ull, 4ull};
        const std::vector<uint8_t> even_aux(16u, 0xABu);
        const auto even_chunk0 = build_synthetic_etpf_chunk(even_descs, even_keys, even_sec, even_aux);
        OVF_EXPECT(even_chunk0.size() == 0xA0u);

        EtpfTable even_table{};
        OVF_EXPECT(parse_etpf_table(even_chunk0, even_table) == Status::Ok);
        OVF_EXPECT(even_table.descriptors.size() == 4u);
        OVF_EXPECT(even_table.descriptors[3].dimension() == EtpfTextureDimension::Texture2DArray);
        OVF_EXPECT(even_table.descriptors[3].depth_or_slices == 8u);
        OVF_EXPECT(even_table.resource_keys == even_keys);
        OVF_EXPECT(even_table.secondary_entries == even_sec);
        OVF_EXPECT(even_table.aux_pool == even_aux);

        // Empty table (N = 0, S_aux = 0) parses cleanly
        std::vector<uint8_t> empty_chunk0(kEtpfHeaderSize, 0);
        write_etpf_header(empty_chunk0, 0, kEtpfMagic, kEtpfVersion, 0u, 0u);
        EtpfTable empty_table{};
        OVF_EXPECT(parse_etpf_table(empty_chunk0, empty_table) == Status::Ok);
        OVF_EXPECT(empty_table.descriptors.empty());
        OVF_EXPECT(empty_table.resource_keys.empty());
        OVF_EXPECT(empty_table.secondary_entries.empty());
        OVF_EXPECT(empty_table.aux_pool.empty());

        // Non-monotonic virt_page rejected with CorruptData (and stale output cleared)
        {
            auto bad_virt_descs = odd_descs;
            bad_virt_descs[0].virt_page = 10u;
            bad_virt_descs[1].virt_page = 20u;
            bad_virt_descs[2].virt_page = 19u;  // Regresses below previous virt_page
            const auto bad_virt_buf =
                build_synthetic_etpf_chunk(bad_virt_descs, odd_keys, odd_sec, odd_aux);
            OVF_EXPECT(parse_etpf_table(bad_virt_buf, table) == Status::CorruptData);
            OVF_EXPECT(table.descriptors.empty());
            OVF_EXPECT(table.resource_keys.empty());
            OVF_EXPECT(table.secondary_entries.empty());
            OVF_EXPECT(table.aux_pool.empty());
        }

        // Unsorted (descending) u64 keys rejected with CorruptData
        {
            const std::vector<uint64_t> descending_keys = {
                0x0000000000001005ull,
                0x0000000100003005ull,
                0x0000000100002005ull,  // Out of order
            };
            const auto bad_keys_buf =
                build_synthetic_etpf_chunk(odd_descs, descending_keys, odd_sec, odd_aux);
            table = even_table;
            OVF_EXPECT(parse_etpf_table(bad_keys_buf, table) == Status::CorruptData);
            OVF_EXPECT(table.descriptors.empty());
            OVF_EXPECT(table.resource_keys.empty());
        }

        // Duplicate u64 keys rejected with CorruptData (strictly ascending required)
        {
            const std::vector<uint64_t> dup_keys = {
                0x0000000000001005ull,
                0x0000000100002005ull,
                0x0000000100002005ull,  // Duplicate key
            };
            const auto dup_keys_buf =
                build_synthetic_etpf_chunk(odd_descs, dup_keys, odd_sec, odd_aux);
            table = even_table;
            OVF_EXPECT(parse_etpf_table(dup_keys_buf, table) == Status::CorruptData);
            OVF_EXPECT(table.descriptors.empty());
            OVF_EXPECT(table.resource_keys.empty());
        }

        // Invalid dimension_flags inside table rejected with UnsupportedFormat
        {
            auto bad_dim_buf = even_chunk0;
            // Overwrite entry 2 w1 (+0x10 + 2*16 + 4 = 0x34) with invalid dimension_flags = 6
            test::write_u32_le(bad_dim_buf, kEtpfHeaderSize + 2u * kEtpfDescriptorSize + 4u, 6u << 21u);
            table = even_table;
            OVF_EXPECT(parse_etpf_table(bad_dim_buf, table) == Status::UnsupportedFormat);
            OVF_EXPECT(table.descriptors.empty());
        }

        // Truncated table or misaligned aux_pool_size rejected and clears stale output
        {
            table = even_table;
            OVF_EXPECT(parse_etpf_table(
                           std::span<const uint8_t>(even_chunk0.data(), even_chunk0.size() - 1u), table) ==
                       Status::OutOfBounds);
            OVF_EXPECT(table.descriptors.empty());

            auto misaligned_aux_buf = even_chunk0;
            test::write_u32_le(misaligned_aux_buf, 12, 8u);
            table = even_table;
            OVF_EXPECT(parse_etpf_table(misaligned_aux_buf, table) == Status::AlignmentError);
            OVF_EXPECT(table.descriptors.empty());
        }
    }

    // 10. Pending stubs return NotImplemented cleanly
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
