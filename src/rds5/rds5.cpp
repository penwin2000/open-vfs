#include "rds5.hpp"

#include <limits>
#include <utility>
#include <open_vfs/macros.hpp>
#include "../vfs_core/vfs_core.hpp"

namespace open_vfs::rds5 {

namespace {

constexpr std::optional<uint64_t> align_up_256(uint64_t value) noexcept {
    constexpr uint64_t kMask = kRds5TableAlignment - 1ull;
    if (value > (std::numeric_limits<uint64_t>::max() - kMask)) {
        return std::nullopt;
    }
    return (value + kMask) & ~kMask;
}

}  // namespace

OPEN_VFS_IMPL
bool validate_rds5_magic(uint32_t raw_magic) noexcept {
    return raw_magic == kRds5Magic;
}

OPEN_VFS_IMPL
Status parse_rds5_header(std::span<const uint8_t> data, Rds5Header& out_header) noexcept {
    if (data.size() < kRds5HeaderSize) {
        return Status::OutOfBounds;
    }

    const auto magic = vfs_core::read_u32_le(data, 0);
    const auto descriptor_count = vfs_core::read_u32_le(data, 4);
    const auto size_and_shift = vfs_core::read_u64_le(data, 8);

    if (!magic || !descriptor_count || !size_and_shift) {
        return Status::OutOfBounds;
    }
    if (!validate_rds5_magic(*magic)) {
        return Status::InvalidMagic;
    }
    if (*descriptor_count < kRds5MinDescriptorCount) {
        return Status::CorruptData;
    }

    const uint64_t total_uncompressed_size = *size_and_shift & kRds5MaxTotalUncompressedSize;
    const uint8_t max_chunk_shift = static_cast<uint8_t>((*size_and_shift >> 56u) & 0xFFu);

    if (max_chunk_shift != kRds5MaxChunkShift) {
        return Status::UnsupportedFormat;
    }
    if ((total_uncompressed_size & (kRds5VirtualPageSize - 1ull)) != 0ull) {
        return Status::AlignmentError;
    }

    out_header.magic = *magic;
    out_header.descriptor_count = *descriptor_count;
    out_header.total_uncompressed_size = total_uncompressed_size;
    out_header.max_chunk_shift = max_chunk_shift;
    return Status::Ok;
}

OPEN_VFS_IMPL
Status parse_rds5_descriptor(std::span<const uint8_t> data,
                             Rds5ChunkDescriptor& out_descriptor) noexcept {
    if (data.size() < kRds5DescriptorSize) {
        return Status::OutOfBounds;
    }

    const auto raw = vfs_core::read_u64_le(data, 0);
    if (!raw.has_value()) {
        return Status::OutOfBounds;
    }

    const uint64_t desc = *raw;
    out_descriptor.raw = desc;
    out_descriptor.disk_offset = (desc & 0x03FFFFFFull) << 8u;
    out_descriptor.codec_id = static_cast<uint8_t>((desc >> 26u) & 0xFull);
    out_descriptor.virtual_offset = ((desc >> 30u) & 0x3FFFFFull) << 12u;
    out_descriptor.trailing_padding = static_cast<uint16_t>((desc >> 52u) & 0xFFFull);
    return Status::Ok;
}

OPEN_VFS_IMPL
Status parse_rds5_descriptor_table(std::span<const uint8_t> container_bytes,
                                   const Rds5Header& header,
                                   std::vector<Rds5ChunkDescriptor>& out_descriptors,
                                   bool validate_full_container) {
    out_descriptors.clear();

    if (!validate_rds5_magic(header.magic)) {
        return Status::InvalidMagic;
    }
    if (header.descriptor_count < kRds5MinDescriptorCount) {
        return Status::CorruptData;
    }
    if (header.max_chunk_shift != kRds5MaxChunkShift) {
        return Status::UnsupportedFormat;
    }
    if (header.total_uncompressed_size > kRds5MaxTotalUncompressedSize) {
        return Status::CorruptData;
    }
    if ((header.total_uncompressed_size & (kRds5VirtualPageSize - 1ull)) != 0ull) {
        return Status::AlignmentError;
    }

    constexpr uint64_t kHeaderSize = static_cast<uint64_t>(kRds5HeaderSize);
    constexpr uint64_t kDescriptorSize = static_cast<uint64_t>(kRds5DescriptorSize);
    const uint64_t descriptor_count = static_cast<uint64_t>(header.descriptor_count);
    if (descriptor_count > (std::numeric_limits<uint64_t>::max() - kHeaderSize) / kDescriptorSize) {
        return Status::OutOfBounds;
    }

    const uint64_t table_end = kHeaderSize + (descriptor_count * kDescriptorSize);
    const auto expected_chunk0_offset = align_up_256(table_end);
    if (!expected_chunk0_offset.has_value()) {
        return Status::OutOfBounds;
    }

    const uint64_t container_size = static_cast<uint64_t>(container_bytes.size());
    if (table_end > container_size) {
        return Status::OutOfBounds;
    }

    out_descriptors.reserve(static_cast<std::size_t>(descriptor_count));
    std::size_t cursor = kRds5HeaderSize;
    for (uint32_t i = 0; i < header.descriptor_count; ++i) {
        Rds5ChunkDescriptor descriptor{};
        const Status status = parse_rds5_descriptor(
            container_bytes.subspan(cursor, kRds5DescriptorSize), descriptor);
        if (status != Status::Ok) {
            out_descriptors.clear();
            return status;
        }
        out_descriptors.push_back(descriptor);
        cursor += kRds5DescriptorSize;
    }

    if (out_descriptors.front().disk_offset != *expected_chunk0_offset) {
        out_descriptors.clear();
        return Status::AlignmentError;
    }
    if (out_descriptors.front().virtual_offset != 0ull) {
        out_descriptors.clear();
        return Status::CorruptData;
    }

    const uint64_t max_chunk_size = header.max_chunk_size();
    const std::size_t chunk_count = out_descriptors.size() - 1u;
    for (std::size_t i = 0; i < chunk_count; ++i) {
        const auto& curr = out_descriptors[i];
        const auto& next = out_descriptors[i + 1u];

        if (next.disk_offset < curr.disk_offset || next.virtual_offset < curr.virtual_offset) {
            out_descriptors.clear();
            return Status::CorruptData;
        }

        const uint64_t disk_span = next.disk_offset - curr.disk_offset;
        if (static_cast<uint64_t>(curr.trailing_padding) > disk_span) {
            out_descriptors.clear();
            return Status::CorruptData;
        }

        const uint64_t uncompressed_size = next.virtual_offset - curr.virtual_offset;
        if (uncompressed_size > max_chunk_size) {
            out_descriptors.clear();
            return Status::CorruptData;
        }
    }

    const auto& sentinel = out_descriptors.back();
    if (sentinel.virtual_offset != header.total_uncompressed_size) {
        out_descriptors.clear();
        return Status::CorruptData;
    }
    if (validate_full_container) {
        if (sentinel.disk_offset > container_size) {
            out_descriptors.clear();
            return Status::OutOfBounds;
        }
        if (sentinel.disk_offset < container_size) {
            out_descriptors.clear();
            return Status::CorruptData;
        }
    }

    return Status::Ok;
}

OPEN_VFS_IMPL
Status parse_rds5_chunk_index(std::span<const uint8_t> container_bytes,
                              const Rds5Header& header,
                              std::vector<Rds5ChunkInfo>& out_chunks,
                              bool validate_full_container) {
    out_chunks.clear();

    std::vector<Rds5ChunkDescriptor> descriptors;
    const Status status = parse_rds5_descriptor_table(
        container_bytes, header, descriptors, validate_full_container);
    if (status != Status::Ok) {
        return status;
    }

    const std::size_t chunk_count = descriptors.size() - 1u;
    out_chunks.reserve(chunk_count);
    for (std::size_t i = 0; i < chunk_count; ++i) {
        const auto& curr = descriptors[i];
        const auto& next = descriptors[i + 1u];
        const uint64_t disk_span = next.disk_offset - curr.disk_offset;

        Rds5ChunkInfo info{};
        info.disk_offset = curr.disk_offset;
        info.disk_span = disk_span;
        info.stored_size = disk_span - static_cast<uint64_t>(curr.trailing_padding);
        info.virtual_offset = curr.virtual_offset;
        info.uncompressed_size = next.virtual_offset - curr.virtual_offset;
        info.trailing_padding = curr.trailing_padding;
        info.codec_id = curr.codec_id;
        out_chunks.push_back(info);
    }

    return Status::Ok;
}

OPEN_VFS_IMPL
Status parse_etpf_header(std::span<const uint8_t> chunk0_data,
                         EtpfHeader& out_header) noexcept {
    if (chunk0_data.size() < kEtpfHeaderSize) {
        return Status::OutOfBounds;
    }

    const auto magic = vfs_core::read_u32_le(chunk0_data, 0);
    const auto version = vfs_core::read_u32_le(chunk0_data, 4);
    const auto entry_count = vfs_core::read_u32_le(chunk0_data, 8);
    const auto aux_pool_size = vfs_core::read_u32_le(chunk0_data, 12);

    if (!magic || !version || !entry_count || !aux_pool_size) {
        return Status::OutOfBounds;
    }
    if (*magic != kEtpfMagic) {
        return Status::InvalidMagic;
    }
    if (*version != kEtpfVersion) {
        return Status::UnsupportedFormat;
    }
    if ((*aux_pool_size % static_cast<uint32_t>(kEtpfAuxPoolAlignment)) != 0u) {
        return Status::AlignmentError;
    }

    constexpr uint64_t kHeaderSize = static_cast<uint64_t>(kEtpfHeaderSize);
    constexpr uint64_t kPerEntrySize = static_cast<uint64_t>(kEtpfTotalBytesPerEntry);
    const uint64_t count64 = static_cast<uint64_t>(*entry_count);
    const uint64_t aux64 = static_cast<uint64_t>(*aux_pool_size);

    if (count64 > (std::numeric_limits<uint64_t>::max() - kHeaderSize) / kPerEntrySize) {
        return Status::OutOfBounds;
    }
    const uint64_t fixed_regions_end = kHeaderSize + (count64 * kPerEntrySize);
    if (aux64 > (std::numeric_limits<uint64_t>::max() - fixed_regions_end)) {
        return Status::OutOfBounds;
    }

    const uint64_t total_span = fixed_regions_end + aux64;
    const uint64_t chunk_size = static_cast<uint64_t>(chunk0_data.size());
    if (total_span > chunk_size) {
        return Status::OutOfBounds;
    }

    out_header.magic = *magic;
    out_header.version = *version;
    out_header.entry_count = *entry_count;
    out_header.aux_pool_size = *aux_pool_size;
    return Status::Ok;
}

OPEN_VFS_IMPL
std::optional<uint32_t> lookup_etpf_chunk_size_pages(uint8_t code) noexcept {
    // Verified 5-bit ETPF chunk_size_codes table (units of 4 KiB pages; bytes = pages * 4096).
    // Code 0 represents an absent/empty slot (0 pages). Unobserved codes return std::nullopt.
    // RDS5 chunk descriptors remain authoritative for exact per-chunk uncompressed_size.
    switch (code) {
        case 0u:  return 0u;    // Absent / empty slot
        case 1u:  return 1u;    //    4 KiB
        case 2u:  return 2u;    //    8 KiB
        case 3u:  return 3u;    //   12 KiB
        case 4u:  return 4u;    //   16 KiB
        case 5u:  return 5u;    //   20 KiB
        case 6u:  return 6u;    //   24 KiB
        case 7u:  return 7u;    //   28 KiB
        case 8u:  return 8u;    //   32 KiB
        case 10u: return 12u;   //   48 KiB
        case 11u: return 16u;   //   64 KiB
        case 12u: return 24u;   //   96 KiB
        case 13u: return 32u;   //  128 KiB
        case 14u: return 48u;   //  192 KiB
        case 15u: return 64u;   //  256 KiB
        case 16u: return 128u;  //  512 KiB
        case 21u: return 18u;   //   72 KiB
        case 22u: return 80u;   //  320 KiB
        case 23u: return 96u;   //  384 KiB
        case 24u: return 144u;  //  576 KiB
        case 26u: return 272u;  // 1088 KiB
        case 27u: return 384u;  // 1536 KiB
        case 28u: return 528u;  // 2112 KiB
        default:  return std::nullopt;  // Unobserved (9, 17..20, 25, 29..31) or > 31
    }
}

OPEN_VFS_IMPL
Status parse_etpf_texture_descriptor(std::span<const uint8_t> data,
                                     EtpfTextureDescriptor& out_descriptor) noexcept {
    if (data.size() < kEtpfDescriptorSize) {
        return Status::OutOfBounds;
    }

    const auto w0 = vfs_core::read_u32_le(data, 0);
    const auto w1 = vfs_core::read_u32_le(data, 4);
    const auto q23 = vfs_core::read_u64_le(data, 8);
    if (!w0 || !w1 || !q23) {
        return Status::OutOfBounds;
    }

    const uint8_t dimension_flags = static_cast<uint8_t>((*w1 >> 21u) & 0x07u);
    if (!is_valid_etpf_dimension(dimension_flags)) {
        return Status::UnsupportedFormat;
    }

    EtpfTextureDescriptor parsed{};
    parsed.raw_w0 = *w0;
    parsed.raw_w1 = *w1;
    parsed.raw_q23 = *q23;

    parsed.width = static_cast<uint16_t>((*w0 & 0x3FFFu) + 1u);
    parsed.height = static_cast<uint16_t>(((*w0 >> 14u) & 0x3FFFu) + 1u);
    parsed.tail_mip_count = static_cast<uint8_t>((*w0 >> 28u) & 0x0Fu);

    parsed.streamed_mip_count = static_cast<uint8_t>(*w1 & 0x07u);
    parsed.dxgi_format = static_cast<uint8_t>((*w1 >> 3u) & 0x7Fu);
    parsed.depth_or_slices = static_cast<uint8_t>(((*w1 >> 10u) & 0x3Fu) + 1u);
    parsed.dimension_flags = dimension_flags;
    parsed.packed_tier_count = static_cast<uint8_t>((*w1 >> 24u) & 0x07u);
    parsed.is_cube_tile_flag = ((*w1 >> 27u) & 0x01u) != 0u;
    parsed.aux_flag_10 = ((*w1 >> 28u) & 0x01u) != 0u;

    parsed.virt_page = static_cast<uint32_t>(*q23 & 0x1FFFFFull);
    parsed.chunk_size_codes = {
        static_cast<uint8_t>((*q23 >> 21u) & 0x1Full),
        static_cast<uint8_t>((*q23 >> 26u) & 0x1Full),
        static_cast<uint8_t>((*q23 >> 31u) & 0x1Full),
        static_cast<uint8_t>((*q23 >> 36u) & 0x1Full),
    };
    parsed.sub64k_chunk_count = static_cast<uint8_t>((*q23 >> 51u) & 0x03ull);
    parsed.tile0_free_pages = static_cast<uint8_t>((*q23 >> 53u) & 0x0Full);

    out_descriptor = parsed;
    return Status::Ok;
}

OPEN_VFS_IMPL
Status parse_etpf_table(std::span<const uint8_t> chunk0_data,
                        EtpfTable& out_table) {
    out_table = EtpfTable{};

    EtpfHeader header{};
    const Status header_status = parse_etpf_header(chunk0_data, header);
    if (header_status != Status::Ok) {
        return header_status;
    }

    const std::size_t entry_count = static_cast<std::size_t>(header.entry_count);
    const std::size_t descriptors_offset = static_cast<std::size_t>(header.descriptors_offset());
    const std::size_t keys_offset = static_cast<std::size_t>(header.keys_offset());
    const std::size_t secondary_offset = static_cast<std::size_t>(header.secondary_offset());
    const std::size_t aux_pool_offset = static_cast<std::size_t>(header.aux_pool_offset());
    const std::size_t aux_pool_size = static_cast<std::size_t>(header.aux_pool_size);

    EtpfTable parsed{};
    parsed.header = header;
    parsed.descriptors.reserve(entry_count);
    parsed.resource_keys.reserve(entry_count);
    parsed.secondary_entries.reserve(entry_count);

    // 1. Primary 16-byte GPU texture descriptors [+0x10, +0x10 + 16*N), monotonic by virt_page
    std::size_t cursor = descriptors_offset;
    for (std::size_t i = 0; i < entry_count; ++i) {
        EtpfTextureDescriptor desc{};
        const Status desc_status = parse_etpf_texture_descriptor(
            chunk0_data.subspan(cursor, kEtpfDescriptorSize), desc);
        if (desc_status != Status::Ok) {
            return desc_status;
        }
        if (i > 0u && desc.virt_page < parsed.descriptors.back().virt_page) {
            return Status::CorruptData;
        }
        parsed.descriptors.push_back(desc);
        cursor += kEtpfDescriptorSize;
    }

    // 2. Strictly ascending u64 resource keys [+0x10 + 16*N, +0x10 + 24*N)
    cursor = keys_offset;
    for (std::size_t i = 0; i < entry_count; ++i) {
        const auto key = vfs_core::read_u64_le(chunk0_data, cursor);
        if (!key.has_value()) {
            return Status::OutOfBounds;
        }
        if (i > 0u && *key <= parsed.resource_keys.back()) {
            return Status::CorruptData;
        }
        parsed.resource_keys.push_back(*key);
        cursor += kEtpfKeySize;
    }

    // 3. Opaque / high-entropy secondary 8-byte/entry region [+0x10 + 24*N, +0x10 + 32*N)
    cursor = secondary_offset;
    for (std::size_t i = 0; i < entry_count; ++i) {
        const auto entry = vfs_core::read_u64_le(chunk0_data, cursor);
        if (!entry.has_value()) {
            return Status::OutOfBounds;
        }
        parsed.secondary_entries.push_back(*entry);
        cursor += kEtpfSecondaryEntrySize;
    }

    // 4. 16-byte-aligned trailing auxiliary pool [+0x10 + 32*N, +0x10 + 32*N + S_aux)
    if (aux_pool_size > 0u) {
        const auto aux_slice = chunk0_data.subspan(aux_pool_offset, aux_pool_size);
        parsed.aux_pool.assign(aux_slice.begin(), aux_slice.end());
    }

    out_table = std::move(parsed);
    return Status::Ok;
}

OPEN_VFS_STUB
Status read_rds5_stream_chunk(std::span<const uint8_t> container_bytes,
                              const Rds5ChunkInfo& chunk,
                              std::vector<uint8_t>& out_raw_chunk) {
    (void)container_bytes;
    (void)chunk;
    out_raw_chunk.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status decompress_rds5_chunk(const Rds5ChunkInfo& chunk,
                             std::span<const uint8_t> raw_chunk,
                             std::vector<uint8_t>& out_plain) {
    (void)chunk;
    (void)raw_chunk;
    out_plain.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status resolve_rds5_dependencies(const std::vector<EtpfResourceDescriptor>& descriptors,
                                 uint64_t root_resource_id,
                                 std::vector<std::size_t>& out_dependency_indices) {
    (void)descriptors;
    (void)root_resource_id;
    out_dependency_indices.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status assemble_rds5_resource(const EtpfResourceDescriptor& descriptor,
                              std::span<const uint8_t> virtual_pages,
                              std::span<const uint8_t> physical_pages,
                              std::vector<uint8_t>& out_image) {
    (void)descriptor;
    (void)virtual_pages;
    (void)physical_pages;
    out_image.clear();
    return Status::NotImplemented;
}

}  // namespace open_vfs::rds5
