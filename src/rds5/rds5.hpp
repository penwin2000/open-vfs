#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <open_vfs/status.hpp>

namespace open_vfs::rds5 {

constexpr uint32_t kRds5Magic = 0x52445335u;         // "RDS5"
constexpr uint32_t kRds5MagicAsciiLe = 0x35534452u;  // {'R','D','S','5'} read as LE u32
constexpr std::size_t kRds5HeaderSize = 32;
constexpr std::size_t kRds5DescriptorSize = 24;

struct Rds5Header {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t descriptor_count = 0;
    uint32_t chunk_count = 0;
    uint64_t descriptor_table_offset = 0;
    uint64_t stream_table_offset = 0;
};

struct Rds5ResourceDescriptor {
    uint64_t resource_id = 0;
    uint32_t type_tag = 0;
    uint32_t flags = 0;
    uint32_t virtual_size = 0;
    uint32_t physical_size = 0;
};

struct Rds5ChunkInfo {
    uint64_t file_offset = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
};

bool validate_rds5_magic(uint32_t raw_magic) noexcept;

Status parse_rds5_header(std::span<const uint8_t> data, Rds5Header& out_header) noexcept;

Status parse_rds5_descriptor(std::span<const uint8_t> data,
                             Rds5ResourceDescriptor& out_descriptor) noexcept;

Status parse_rds5_descriptor_table(std::span<const uint8_t> container_bytes,
                                   const Rds5Header& header,
                                   std::vector<Rds5ResourceDescriptor>& out_descriptors);

Status parse_rds5_chunk_index(std::span<const uint8_t> container_bytes,
                              const Rds5Header& header,
                              std::vector<Rds5ChunkInfo>& out_chunks);

Status read_rds5_stream_chunk(std::span<const uint8_t> container_bytes,
                              const Rds5ChunkInfo& chunk,
                              std::vector<uint8_t>& out_raw_chunk);

Status decompress_rds5_chunk(const Rds5ChunkInfo& chunk,
                             std::span<const uint8_t> raw_chunk,
                             std::vector<uint8_t>& out_plain);

Status resolve_rds5_dependencies(const std::vector<Rds5ResourceDescriptor>& descriptors,
                                 uint64_t root_resource_id,
                                 std::vector<std::size_t>& out_dependency_indices);

Status assemble_rds5_resource(const Rds5ResourceDescriptor& descriptor,
                              std::span<const uint8_t> virtual_pages,
                              std::span<const uint8_t> physical_pages,
                              std::vector<uint8_t>& out_image);

}  // namespace open_vfs::rds5
