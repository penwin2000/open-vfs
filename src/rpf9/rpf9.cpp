#include "rpf9.hpp"

#include <limits>
#include <open_vfs/macros.hpp>
#include "../vfs_core/vfs_core.hpp"

namespace open_vfs::rpf9 {

OPEN_VFS_IMPL
bool validate_rpf9_magic(uint32_t raw_magic) noexcept {
    return raw_magic == kRpf9Magic || raw_magic == kRpf9MagicAsciiLe;
}

OPEN_VFS_IMPL
Status parse_rpf9_header(std::span<const uint8_t> data, Rpf9Header& out_header) noexcept {
    if (data.size() < kRpf9HeaderSize) {
        return Status::OutOfBounds;
    }

    const auto magic = vfs_core::read_u32_le(data, 0);
    const auto entry_count = vfs_core::read_u32_le(data, 4);
    const auto names_length = vfs_core::read_u32_le(data, 8);
    const auto encryption_flags = vfs_core::read_u32_le(data, 12);
    const auto toc_offset = vfs_core::read_u64_le(data, 16);
    const auto names_offset = vfs_core::read_u64_le(data, 24);

    if (!magic || !entry_count || !names_length || !encryption_flags || !toc_offset || !names_offset) {
        return Status::OutOfBounds;
    }
    if (!validate_rpf9_magic(*magic)) {
        return Status::InvalidMagic;
    }
    if (!vfs_core::is_aligned_16(*toc_offset) || !vfs_core::is_aligned_16(*names_offset)) {
        return Status::AlignmentError;
    }

    out_header.magic = *magic;
    out_header.entry_count = *entry_count;
    out_header.names_length = *names_length;
    out_header.encryption_flags = *encryption_flags;
    out_header.toc_offset = *toc_offset;
    out_header.names_offset = *names_offset;
    return Status::Ok;
}

OPEN_VFS_IMPL
Status parse_rpf9_toc_entry(std::span<const uint8_t> entry_bytes, Rpf9TocEntry& out_entry) noexcept {
    if (entry_bytes.size() < kRpf9TocEntrySize) {
        return Status::OutOfBounds;
    }

    const auto name_offset = vfs_core::read_u32_le(entry_bytes, 0);
    const auto flags = vfs_core::read_u32_le(entry_bytes, 4);
    const auto offset_or_index = vfs_core::read_u32_le(entry_bytes, 8);
    const auto size_or_count = vfs_core::read_u32_le(entry_bytes, 12);

    if (!name_offset || !flags || !offset_or_index || !size_or_count) {
        return Status::OutOfBounds;
    }

    out_entry.name_offset = *name_offset;
    out_entry.flags = *flags;
    out_entry.offset_or_index = *offset_or_index;
    out_entry.size_or_count = *size_or_count;
    out_entry.is_directory = ((*flags) & 0x80000000u) != 0u;
    return Status::Ok;
}

OPEN_VFS_IMPL
Status parse_rpf9_toc_table(std::span<const uint8_t> archive_bytes,
                            const Rpf9Header& header,
                            std::vector<Rpf9TocEntry>& out_entries) {
    out_entries.clear();

    if (!validate_rpf9_magic(header.magic)) {
        return Status::InvalidMagic;
    }
    if (!vfs_core::is_aligned_16(header.toc_offset)) {
        return Status::AlignmentError;
    }

    const uint64_t archive_size = static_cast<uint64_t>(archive_bytes.size());
    if (header.toc_offset > archive_size) {
        return Status::OutOfBounds;
    }

    const uint64_t entry_count = static_cast<uint64_t>(header.entry_count);
    constexpr uint64_t kEntrySize = static_cast<uint64_t>(kRpf9TocEntrySize);
    if (entry_count > (std::numeric_limits<uint64_t>::max() / kEntrySize)) {
        return Status::OutOfBounds;
    }

    const uint64_t table_size = entry_count * kEntrySize;
    if (table_size > archive_size - header.toc_offset) {
        return Status::OutOfBounds;
    }

    out_entries.reserve(static_cast<std::size_t>(entry_count));
    std::size_t cursor = static_cast<std::size_t>(header.toc_offset);
    for (uint32_t i = 0; i < header.entry_count; ++i) {
        Rpf9TocEntry entry{};
        const Status status = parse_rpf9_toc_entry(
            archive_bytes.subspan(cursor, kRpf9TocEntrySize), entry);
        if (status != Status::Ok) {
            out_entries.clear();
            return status;
        }
        out_entries.push_back(entry);
        cursor += kRpf9TocEntrySize;
    }

    return Status::Ok;
}

OPEN_VFS_STUB
Status resolve_rpf9_name_table(std::span<const uint8_t> names_heap,
                               const std::vector<Rpf9TocEntry>& entries,
                               std::vector<std::string>& out_names) {
    (void)names_heap;
    (void)entries;
    out_names.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status locate_rpf9_entry_by_path(const std::vector<Rpf9TocEntry>& entries,
                                 const std::vector<std::string>& names,
                                 std::string_view virtual_path,
                                 std::size_t& out_index) {
    (void)entries;
    (void)names;
    (void)virtual_path;
    out_index = 0;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status parse_rpf9_block_header(std::span<const uint8_t> block_bytes,
                               Rpf9BlockHeader& out_block) noexcept {
    (void)block_bytes;
    out_block = Rpf9BlockHeader{};
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status decompress_rpf9_block(const Rpf9BlockHeader& block_header,
                             std::span<const uint8_t> compressed_payload,
                             std::vector<uint8_t>& out_plain) {
    (void)block_header;
    (void)compressed_payload;
    out_plain.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status decrypt_rpf9_payload(uint32_t encryption_flags,
                            std::span<uint8_t> payload_in_out) {
    (void)encryption_flags;
    (void)payload_in_out;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status extract_rpf9_entry_bytes(std::span<const uint8_t> archive_bytes,
                                const Rpf9Header& header,
                                const Rpf9TocEntry& entry,
                                std::vector<uint8_t>& out_payload) {
    (void)archive_bytes;
    (void)header;
    (void)entry;
    out_payload.clear();
    return Status::NotImplemented;
}

}  // namespace open_vfs::rpf9
