#include "rds5.hpp"

#include <limits>
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
    const auto secondary_offset = vfs_core::read_u32_le(chunk0_data, 12);

    if (!magic || !version || !entry_count || !secondary_offset) {
        return Status::OutOfBounds;
    }
    if (*magic != kEtpfMagic) {
        return Status::InvalidMagic;
    }
    if (*version != kEtpfVersion) {
        return Status::UnsupportedFormat;
    }

    constexpr uint64_t kHeaderSize = static_cast<uint64_t>(kEtpfHeaderSize);
    constexpr uint64_t kEntrySize = static_cast<uint64_t>(kEtpfDescriptorSize);
    const uint64_t count64 = static_cast<uint64_t>(*entry_count);
    if (count64 > (std::numeric_limits<uint64_t>::max() - kHeaderSize) / kEntrySize) {
        return Status::OutOfBounds;
    }

    const uint64_t primary_table_end = kHeaderSize + (count64 * kEntrySize);
    const uint64_t chunk_size = static_cast<uint64_t>(chunk0_data.size());
    if (primary_table_end > chunk_size) {
        return Status::OutOfBounds;
    }

    const uint64_t secondary64 = static_cast<uint64_t>(*secondary_offset);
    if (secondary64 > chunk_size) {
        return Status::OutOfBounds;
    }
    if (secondary64 < primary_table_end) {
        return Status::CorruptData;
    }

    out_header.magic = *magic;
    out_header.version = *version;
    out_header.entry_count = *entry_count;
    out_header.secondary_table_offset = *secondary_offset;
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
