#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <memory>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <vector>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <model_path.h>
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"

#include "audio_codec.h"
#include "audio_processor.h"
#include "processors/audio_debugger.h"
#include "wake_word.h"
#include "protocol.h"
#include "ogg_demuxer.h"
#include "audio_items.h"
#include "audio_pipeline.h"

/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Processors] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use one task for MIC / Speaker / Processors, and one task for Opus Encoder / Opus Decoder.
 * 
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM packets.
 * 
 */

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_AUDIO_TESTING_RUNNING      (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING          (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY         (1 << 3)
#define AS_EVENT_RAW_CAPTURE_RUNNING        (1 << 4)

#define AS_OPUS_GET_FRAME_DRU_ENUM(duration_ms)                   \
    ((duration_ms) == 5 ? ESP_OPUS_ENC_FRAME_DURATION_5_MS :      \
     (duration_ms) == 10 ? ESP_OPUS_ENC_FRAME_DURATION_10_MS :    \
     (duration_ms) == 20 ? ESP_OPUS_ENC_FRAME_DURATION_20_MS :    \
     (duration_ms) == 40 ? ESP_OPUS_ENC_FRAME_DURATION_40_MS :    \
     (duration_ms) == 60 ? ESP_OPUS_ENC_FRAME_DURATION_60_MS :    \
     (duration_ms) == 80 ? ESP_OPUS_ENC_FRAME_DURATION_80_MS :    \
     (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS :  \
     (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS : -1)

#define AS_OPUS_ENC_CONFIG() {                                                                                    \
        .sample_rate        = ESP_AUDIO_SAMPLE_RATE_16K,                                                          \
        .channel            = ESP_AUDIO_MONO,                                                                     \
        .bits_per_sample    = ESP_AUDIO_BIT16,                                                                    \
        .bitrate            = ESP_OPUS_BITRATE_AUTO,                                                              \
        .frame_duration     = (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(OPUS_FRAME_DURATION_MS),  \
        .application_mode   = ESP_OPUS_ENC_APPLICATION_AUDIO,                                                     \
        .complexity         = 0,                                                                                  \
        .enable_fec         = false,                                                                              \
        .enable_dtx         = true,                                                                               \
        .enable_vbr         = true,                                                                               \
    }

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
    // StackChan FW-A2 (design §2.4, AutoStop): no server audio is in flight any more after a
    // RequestPlaybackDrain(). Called under audio_queue_mutex_: only post (UiController::Post).
    std::function<void(void)> on_playback_drained;
};

// AudioTaskType, RawCaptureFrame and AudioTask: audio_items.h (StackChan FW-A2)

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    bool IsIdle();
    void WaitForPlaybackQueueEmpty();
    bool IsWakeWordRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING; }
    bool IsAudioProcessorRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING; }
    bool IsRawCaptureRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_RAW_CAPTURE_RUNNING; }
    bool IsAfeWakeWord();

    void EnableWakeWordDetection(bool enable);
    void EnableVoiceProcessing(bool enable);
    void EnableRawCapture(bool enable);
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    // StackChan FW-A (design §2.1.2): server audio only. Dropped (and counted) while
    // server audio is not accepted, i.e. from ResetDecoder() until AcceptServerAudio(true).
    // Local sounds (PlaySound) keep using PushPacketToDecodeQueue and are never gated.
    bool PushServerPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet);
    void AcceptServerAudio(bool accept);
    // Diagnostic count of server packets dropped by the gate since the last call; resets it.
    uint32_t TakeServerAudioRejected();
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    // StackChan FW-A (design §2.1.1): clears the decode/playback queues, bumps the playback
    // generation (so audio decoded from an already-popped packet is discarded) and stops
    // accepting server audio, all under one lock. Returns the number of queued items cleared.
    uint32_t ResetDecoder();
    void SetModelsList(srmodel_list_t* models_list);

    // StackChan FW-A2 (plan 2B-2a; design §2.2, §2.4, §6.1). Built in, not called until plan 2B-2b
    // switches the application over (FW-A's AcceptServerAudio / ResetDecoder path stays until then).
    // The gate's AudioSink: a stop clears only when server audio accepted since the previous stop
    // is still unwritten; ClearForListening and the start from Idle clear unconditionally.
    stackchan::gate::AudioSink* playback_sink() { return &playback_sink_; }
    // The gate's R4 push: the decode queue takes the server packet or not (full: counted, the
    // packet stays with the caller). Never waits.
    bool PushServerAudio(std::unique_ptr<AudioStreamPacket>& packet);
    // AutoStop: on_playback_drained once no server audio is in flight (at once if none is).
    void RequestPlaybackDrain();
    // The popup at the start of listening: never waits; false when a packet did not fit (counted).
    bool PlaySoundNoWait(const std::string_view& sound);
    // stat (contract §5.1, design §6.1-6.2)
    stackchan::audio::PlaybackBook PlaybackBookSnapshot();
    stackchan::audio::PipelineStats PlaybackStats();

private:
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    std::unique_ptr<AudioDebugger> audio_debugger_;
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    std::mutex decoder_mutex_;
    std::mutex input_resampler_mutex_;
    esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;
    
    // Encoder/Decoder state
    int encoder_sample_rate_ = 16000;
    int encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int encoder_frame_size_ = 0;
    int encoder_outbuf_size_ = 0;
    int decoder_sample_rate_ = 0;
    int decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int decoder_frame_size_ = 0;
    DebugStatistics debug_statistics_;
    srmodel_list_t* models_list_ = nullptr;

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    // StackChan FW-A2: the decode and playback queues and the book (one generation, bumped by
    // every clear), under audio_queue_mutex_
    stackchan::audio::AudioPipeline pipeline_{
        stackchan::audio::PipelineLimits{MAX_DECODE_PACKETS_IN_QUEUE, MAX_PLAYBACK_TASKS_IN_QUEUE},
        [this]() {
            if (callbacks_.on_playback_drained) callbacks_.on_playback_drained();
        },
        [this]() { ResetDecoderStateLocked(); }};
    stackchan::audio::PipelineSink playback_sink_{&audio_queue_mutex_, &pipeline_,
                                                  [this]() { audio_queue_cv_.notify_all(); }};
    bool accept_server_audio_ = false;              // guarded by audio_queue_mutex_
    uint32_t server_audio_rejected_ = 0;            // diagnostic; not part of abort dropped_ms
    std::mutex raw_capture_mutex_;
    std::atomic<uint32_t> raw_capture_generation_{0};
    std::unique_ptr<std::vector<int16_t>> raw_capture_buffer_;
    size_t raw_capture_buffer_samples_ = 0;
    std::unique_ptr<std::deque<std::unique_ptr<RawCaptureFrame>>> raw_capture_free_frames_;
    // For server AEC
    std::deque<uint32_t> timestamp_queue_;

    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    bool voice_detected_ = false;
    bool service_stopped_ = true;
    bool audio_input_need_warmup_ = false;

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
    void FeedRawCapture(std::vector<int16_t>&& data);
    bool PushRawCaptureFrameToEncodeQueue(uint32_t generation, const int16_t* pcm);
    void ReturnRawCaptureFrame(std::unique_ptr<RawCaptureFrame> frame, uint32_t generation);
    bool IsRawCaptureGenerationCurrent(uint32_t generation) const;
    void AllocateRawCaptureStorage();
    void ReleaseRawCaptureStorage();
    void DropRawCaptureQueuedDataLocked();
    void ResetRawCaptureBuffer();
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    // Every clear of the pipeline, under audio_queue_mutex_: the opus decoder's state, the
    // server-AEC timestamps and the testing queue (what ResetDecoder cleared besides the queues)
    void ResetDecoderStateLocked();
    void PlaySoundImpl(const std::string_view& sound, bool wait, bool* all_queued);
    void CheckAndUpdateAudioPowerState();
};

#endif
