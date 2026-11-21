#include "test_harness.hpp"
#include "audio/audio.hpp"

#include <array>
#include <vector>

int main() {
    using namespace open_vfs;
    using namespace open_vfs::audio;

    // 1. Audio format validation
    AudioFormat fmt{};
    OVF_EXPECT(validate_audio_format(fmt));

    AudioFormat bad_rate = fmt;
    bad_rate.sample_rate = 100;
    OVF_EXPECT(!validate_audio_format(bad_rate));

    // 2. Software float32 PCM mixing with gain and clamping
    const std::array<float, 4> a{0.5f, -0.5f, 0.9f, -0.8f};
    const std::array<float, 4> b{0.25f, 0.5f, 0.6f, -0.7f};
    std::array<float, 4> dst{};
    OVF_EXPECT(mix_pcm_buffers_f32(a, b, 1.0f, 1.0f, dst) == Status::Ok);
    OVF_EXPECT_NEAR(dst[0], 0.75f, 1e-5f);
    OVF_EXPECT_NEAR(dst[1], 0.0f, 1e-5f);
    OVF_EXPECT_NEAR(dst[2], 1.0f, 1e-5f);   // clamped to +1.0
    OVF_EXPECT_NEAR(dst[3], -1.0f, 1e-5f);  // clamped to -1.0

    // 3. Pending WASAPI & decoder stubs
    AudioDeviceContext ctx{};
    OVF_EXPECT(init_wasapi_device(fmt, ctx) == Status::NotImplemented);
    OVF_EXPECT(init_wasapi_device(bad_rate, ctx) == Status::InvalidArgument);

    std::array<uint8_t, 32> synthetic_hdr{};
    AudioStreamHeader stream_hdr{};
    OVF_EXPECT(parse_audio_stream_header(synthetic_hdr, stream_hdr) == Status::NotImplemented);

    std::vector<float> decoded;
    OVF_EXPECT(decode_adpcm_block(synthetic_hdr, 2, decoded) == Status::NotImplemented);

    return open_vfs::test::g_failures == 0 ? 0 : 1;
}
