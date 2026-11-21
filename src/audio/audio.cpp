#include "audio.hpp"

#include <algorithm>
#include <open_vfs/macros.hpp>

namespace open_vfs::audio {

OPEN_VFS_IMPL
bool validate_audio_format(const AudioFormat& format) noexcept {
    if (format.sample_rate < 8000 || format.sample_rate > 192000) {
        return false;
    }
    if (format.channels == 0 || format.channels > 8) {
        return false;
    }
    if (format.bits_per_sample != 16 && format.bits_per_sample != 24 && format.bits_per_sample != 32) {
        return false;
    }
    return format.codec != AudioCodec::Unknown;
}

OPEN_VFS_IMPL
Status mix_pcm_buffers_f32(std::span<const float> src_a,
                           std::span<const float> src_b,
                           float gain_a,
                           float gain_b,
                           std::span<float> dst) noexcept {
    if (src_a.size() != src_b.size() || src_a.size() != dst.size()) {
        return Status::InvalidArgument;
    }
    for (std::size_t i = 0; i < dst.size(); ++i) {
        const float mixed = (src_a[i] * gain_a) + (src_b[i] * gain_b);
        dst[i] = std::clamp(mixed, -1.0f, 1.0f);
    }
    return Status::Ok;
}

OPEN_VFS_STUB
Status init_wasapi_device(const AudioFormat& desired_format, AudioDeviceContext& out_ctx) {
    out_ctx = AudioDeviceContext{};
    if (!validate_audio_format(desired_format)) {
        return Status::InvalidArgument;
    }
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status shutdown_wasapi_device(AudioDeviceContext& ctx) {
    ctx.running = false;
    ctx.client_handle = nullptr;
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status parse_audio_stream_header(std::span<const uint8_t> header_bytes,
                                 AudioStreamHeader& out_header) noexcept {
    (void)header_bytes;
    out_header = AudioStreamHeader{};
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status decode_adpcm_block(std::span<const uint8_t> encoded_block,
                          uint16_t channels,
                          std::vector<float>& out_pcm_f32) {
    (void)encoded_block;
    (void)channels;
    out_pcm_f32.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status decode_opus_packet(std::span<const uint8_t> packet_bytes,
                          uint32_t sample_rate,
                          uint16_t channels,
                          std::vector<float>& out_pcm_f32) {
    (void)packet_bytes;
    (void)sample_rate;
    (void)channels;
    out_pcm_f32.clear();
    return Status::NotImplemented;
}

OPEN_VFS_STUB
Status submit_audio_voice(AudioDeviceContext& ctx,
                          uint32_t voice_id,
                          std::span<const float> interleaved_pcm) {
    (void)ctx;
    (void)voice_id;
    (void)interleaved_pcm;
    return Status::NotImplemented;
}

}  // namespace open_vfs::audio
