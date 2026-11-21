#pragma once

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace open_vfs::test {

inline int g_failures = 0;

inline void write_u32_le(std::vector<uint8_t>& buf, std::size_t offset, uint32_t value) {
    buf[offset + 0] = static_cast<uint8_t>(value & 0xFFu);
    buf[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
    buf[offset + 2] = static_cast<uint8_t>((value >> 16u) & 0xFFu);
    buf[offset + 3] = static_cast<uint8_t>((value >> 24u) & 0xFFu);
}

inline void write_u64_le(std::vector<uint8_t>& buf, std::size_t offset, uint64_t value) {
    write_u32_le(buf, offset, static_cast<uint32_t>(value & 0xFFFFFFFFull));
    write_u32_le(buf, offset + 4, static_cast<uint32_t>((value >> 32u) & 0xFFFFFFFFull));
}

}  // namespace open_vfs::test

#define OVF_EXPECT(cond)                                                                      \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " -> " << #cond << "\n"; \
            ++open_vfs::test::g_failures;                                                     \
        }                                                                                     \
    } while (false)

#define OVF_EXPECT_NEAR(a, b, eps) OVF_EXPECT(std::fabs((a) - (b)) <= (eps))
