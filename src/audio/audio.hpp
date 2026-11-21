#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <open_vfs/status.hpp>

namespace open_vfs::audio {

enum class AudioCodec : uint32_t {
    Unknown = 0,
    Pcm16 = 1,
    PcmF32 = 2,
    Adpcm = 3,
    Opus = 4,
};

struct AudioFormat {
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    uint16_t bits_per_sample = 32;
    AudioCodec codec = AudioCodec::PcmF32;
};

struct AudioStreamHeader {
    AudioFormat format;
    uint64_t total_frames = 0;
    uint32_t block_align = 0;
};

struct AudioDeviceContext {
    void* client_handle = nullptr;
    AudioFormat active_format;
    bool running = false;
};

bool validate_audio_format(const AudioFormat& format) noexcept;

Status mix_pcm_buffers_f32(std::span<const float> src_a,
                           std::span<const float> src_b,
                           float gain_a,
                           float gain_b,
                           std::span<float> dst) noexcept;

Status init_wasapi_device(const AudioFormat& desired_format, AudioDeviceContext& out_ctx);

Status shutdown_wasapi_device(AudioDeviceContext& ctx);

Status parse_audio_stream_header(std::span<const uint8_t> header_bytes,
                                 AudioStreamHeader& out_header) noexcept;

Status decode_adpcm_block(std::span<const uint8_t> encoded_block,
                          uint16_t channels,
                          std::vector<float>& out_pcm_f32);

Status decode_opus_packet(std::span<const uint8_t> packet_bytes,
                          uint32_t sample_rate,
                          uint16_t channels,
                          std::vector<float>& out_pcm_f32);

Status submit_audio_voice(AudioDeviceContext& ctx,
                          uint32_t voice_id,
                          std::span<const float> interleaved_pcm);

}  // namespace open_vfs::audio
