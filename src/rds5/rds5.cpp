#include "rds5.hpp"

#include <open_vfs/macros.hpp>
#include "../vfs_core/vfs_core.hpp"

namespace open_vfs::rds5 {

OPEN_VFS_IMPL
bool validate_rds5_magic(uint32_t raw_magic) noexcept {
    return raw_magic == kRds5Magic || raw_magic == kRds5MagicAsciiLe;
}

OPEN_VFS_IMPL
Status parse_rds5_header(std::span<const uint8_t> data, Rds5Header& out_header) noexcept {
    if (data.size() < kRds5HeaderSize) {
        return Status::OutOfBounds;
    }

    const auto magic = vfs_core::read_u32_le(data, 0);
    const auto version = vfs_core::read_u32_le(data, 4);
    const auto descriptor_count = vfs_core::read_u32_le(data, 8);
    const auto chunk_count = vfs_core::read_u32_le(data, 12);
    const auto descriptor_offset = vfs_core::read_u64_le(data, 16);
    const auto stream_offset = vfs_core::read_u64_le(data, 24);

    if (!magic || !version || !descriptor_count || !chunk_count || !descriptor_offset || !stream_offset) {
        return Status::OutOfBounds;
    }
    if (!validate_rds5_magic(*magic)) {
        return Status::InvalidMagic;
    }
    if (!vfs_core::is_aligned_16(*descriptor_offset) || !vfs_core::is_aligned_16(*stream_offset)) {
        return Status::AlignmentError;
    }

    out_header.magic = *magic;
    out_header.version = *version;
    out_header.descriptor_count = *descriptor_count;
    out_header.chunk_count = *chunk_count;
    out_header.descriptor_table_offset = *descriptor_offset;
    out_header.stream_table_offset = *stream_offset;
    return Status::Ok;
}

OPEN_VFS_IMPL
Status parse_rds5_descriptor(std::span<const uint8_t> data,
                             Rds5ResourceDescriptor& out_descriptor) noexcept {
    if (data.size() < kRds5DescriptorSize) {
        return Status::OutOfBounds;
    }

    const auto resource_id = vfs_core::read_u64_le(data, 0);
    const auto type_tag = vfs_core::read_u32_le(data, 8);
    const auto flags = vfs_core::read_u32_le(data, 12);
    const auto virtual_size = vfs_core::read_u32_le(data, 16);
    const auto physical_size = vfs_core::read_u32_le(data, 20);

    if (!resource_id || !type_tag || !flags || !virtual_size || !physical_size) {
        return Status::OutOfBounds;
    }

    out_descriptor.resource_id = *resource_id;
    out_descriptor.type_tag = *type_tag;
    out_descriptor.flags = *flags;
    out_descriptor.virtual_size = *virtual_size;
    out_descriptor.physical_size = *physical_size;
    return Status::Ok;
}

OPEN_VFS_STUB
Status parse_rds5_descriptor_table(std::span<const uint8_t> container_bytes,
                                   const Rds5Header& header,
                                   std::vector<Rds5ResourceDescriptor>& out_descriptors) {
    (void)container_bytes;
    (void)header;
    out_descriptors.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status parse_rds5_chunk_index(std::span<const uint8_t> container_bytes,
                              const Rds5Header& header,
                              std::vector<Rds5ChunkInfo>& out_chunks) {
    (void)container_bytes;
    (void)header;
    out_chunks.clear();
    return Status::NotImplemented;
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
Status resolve_rds5_dependencies(const std::vector<Rds5ResourceDescriptor>& descriptors,
                                 uint64_t root_resource_id,
                                 std::vector<std::size_t>& out_dependency_indices) {
    (void)descriptors;
    (void)root_resource_id;
    out_dependency_indices.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status assemble_rds5_resource(const Rds5ResourceDescriptor& descriptor,
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
