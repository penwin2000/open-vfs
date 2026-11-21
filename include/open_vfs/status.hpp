#pragma once

#include <cstdint>
#include <string_view>

namespace open_vfs {

enum class Status : uint32_t {
    Ok = 0,
    NotImplemented = 1,
    InvalidArgument = 2,
    OutOfBounds = 3,
    InvalidMagic = 4,
    AlignmentError = 5,
    NotFound = 6,
    IoError = 7,
    CorruptData = 8,
    UnsupportedFormat = 9,
    DeviceUnavailable = 10,
    ErrUnsupportedDataLayout = 11,
};

constexpr std::string_view status_to_string(Status status) noexcept {
    switch (status) {
        case Status::Ok:                       return "Ok";
        case Status::NotImplemented:           return "NotImplemented";
        case Status::InvalidArgument:          return "InvalidArgument";
        case Status::OutOfBounds:              return "OutOfBounds";
        case Status::InvalidMagic:             return "InvalidMagic";
        case Status::AlignmentError:           return "AlignmentError";
        case Status::NotFound:                 return "NotFound";
        case Status::IoError:                  return "IoError";
        case Status::CorruptData:              return "CorruptData";
        case Status::UnsupportedFormat:        return "UnsupportedFormat";
        case Status::DeviceUnavailable:        return "DeviceUnavailable";
        case Status::ErrUnsupportedDataLayout: return "ErrUnsupportedDataLayout";
    }
    return "Unknown";
}

}  // namespace open_vfs
