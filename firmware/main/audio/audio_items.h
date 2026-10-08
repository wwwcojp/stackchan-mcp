// StackChan FW-A2 plan 2B-2a (design §6.1): the items that move through the AudioService's queues,
// kept free of ESP-IDF so the playback pipeline (audio_pipeline) is host-tested. Server audio
// carries its origin (the gate's stop serial when it was queued) from the decode queue to the
// output; local sounds (popups, the audio-testing recording) carry the local origin.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "audio_origin.h"

struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    // Local-only marker used before transport serialization.
    uint32_t raw_capture_generation = 0;
    std::vector<uint8_t> payload;
    // StackChan FW-A2: set by the pipeline when the packet enters the decode queue
    stackchan::audio::Origin origin;
};

enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct RawCaptureFrame {
    std::vector<int16_t> pcm;
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    std::unique_ptr<RawCaptureFrame> raw_capture_frame;
    uint32_t timestamp = 0;
    uint32_t raw_capture_generation = 0;
    // StackChan FW-A2: the decoded packet's origin
    stackchan::audio::Origin origin;
};
