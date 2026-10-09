// StackChan FW-A2 plan 2B-2a: the real gate, the AudioService's pipeline (through PipelineSink) and
// the UiController wired as plan 2B-2b wires them: the gate's GateChanged and the pipeline's
// PlaybackDrained go into one UI list, the UI's ports call the gate and the pipeline. Plan 2A
// follow-up 1 (Claude review 146 Minor 1): a start that clears must tell GateChanged before the
// clear can tell PlaybackDrained.
#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "audio_pipeline.h"
#include "playback_gate.h"
#include "send_queue.h"
#include "ui_controller.h"

namespace a = stackchan::audio;
namespace g = stackchan::gate;
namespace n = stackchan::net;
namespace u = stackchan::ui;
namespace w = stackchan::wire;

namespace {

constexpr uint64_t E = 7;

struct Wiring : u::UiPorts {
    int64_t now = 1'000'000;
    std::mutex audio_mu;
    std::vector<std::string> log;
    u::UiController* ui = nullptr;
    a::AudioPipeline pipeline{{}, [this] {
                                  u::Event ev;
                                  ev.kind = u::EvKind::kPlaybackDrained;
                                  ui->Post(ev);
                              }};
    a::PipelineSink sink{&audio_mu, &pipeline, nullptr};
    n::SendQueue audio_q{n::kAudioLimits};
    n::SendQueue ctrl_q{n::kCtrlLimits};
    g::PlaybackGate gate{g::GatePorts{
        &sink, &audio_q, &ctrl_q, [this] { return now; },
        [this](uint64_t e, bool spk, uint32_t rev) {
            u::Event ev;
            ev.kind = u::EvKind::kGateChanged;
            ev.e = e;
            ev.spk = spk;
            ev.rev = rev;
            ui->Post(ev);
        },
        [this](uint64_t e, bool spk, uint32_t rev) {
            u::Event ev;
            ev.kind = u::EvKind::kLinkUp;
            ev.e = e;
            ev.spk = spk;
            ev.rev = rev;
            ui->Post(ev);
        },
        [this](uint64_t, stackchan::link::EndReason, bool) { log.push_back("end_pair"); }}};
    DeviceState state = kDeviceStateIdle;
    u::UiController controller{this, u::UiConfig{}};

    Wiring() {
        ui = &controller;
        audio_q.Open(E);
        ctrl_q.Open(E);
    }

    void Drain() {
        while (controller.ProcessOne()) {
        }
    }
    void Resync() {
        u::Event ev;
        ev.kind = u::EvKind::kResync;
        controller.Post(ev);
    }
    void TtsStart(uint32_t gen, uint32_t aborted = 0) {
        w::TtsStart t;
        t.gen = gen;
        t.aborted_gen = aborted;
        t.dev_abort_seen = 0;
        gate.OnTtsStart(E, t);
    }
    bool ServerAudio() {
        bool pushed = false;
        gate.OnServerAudio(E, [&] {
            std::unique_lock<std::mutex> lk(audio_mu);
            auto p = std::make_unique<AudioStreamPacket>();
            pushed = pipeline.PushServer(lk, p);
            return pushed;
        });
        return pushed;
    }
    void GwListenAuto() {
        w::GwListen l;
        l.start = true;
        l.mode = w::ListenMode::kAutoStop;
        controller.Post(u::GwListenEvent(E, l));
    }
    std::vector<std::string> Take() {
        auto out = log;
        log.clear();
        return out;
    }

    // UiPorts
    DeviceState CurrentState() override { return state; }
    bool SetDeviceState(DeviceState s) override {
        state = s;
        log.push_back("state " + std::to_string(s));
        return true;
    }
    g::TouchResult GateTouch(uint64_t e, w::ListenMode m, w::DeviceAbortReason r) override {
        log.push_back("gate_touch");
        return gate.OnTouch(e, m, r);
    }
    void ClearForListening() override { gate.ClearForListening(); }
    void SendListenStart(uint64_t, u::Mode) override { log.push_back("listen_start"); }
    void SendListenStop(uint64_t) override { log.push_back("listen_stop"); }
    void SendWakeWord(uint64_t) override {}
    void MicOn(u::Profile) override { log.push_back("mic_on"); }
    void MicOff() override { log.push_back("mic_off"); }
    void PlayPopup() override { log.push_back("popup"); }
    void RequestDrain() override {
        log.push_back("drain");
        std::unique_lock<std::mutex> lk(audio_mu);
        pipeline.RequestDrain(lk);
    }
    void WakeDetect(bool) override {}
    void ArmTimer(uint32_t) override {}
    void CancelTimer() override {}
    void ShowWaitingForLink() override { log.push_back("waiting"); }
    void StopMouth() override {}
    void StartMouth() override {}
    void SetListeningLed(bool) override {}
    void SetPerformance(bool) override {}
};

// Plan 2A follow-up 1: tts start -> server audio -> tts stop -> GwListen(start, auto) -> a new tts
// start. The new playback's start clears the old audio; the UI must see Speaking before the drain,
// so it never opens the mic or plays the popup at the head of the new playback.
TEST(PlaybackWiring, ANewPlaybackIsSeenBeforeTheDrainItsClearMeets) {
    Wiring wr;
    ASSERT_EQ(wr.gate.Bind(E, "s"), g::Outcome::kBound);
    wr.Resync();
    ASSERT_TRUE(wr.gate.PostLinkUp(E));
    wr.Drain();
    wr.TtsStart(1);
    ASSERT_TRUE(wr.ServerAudio());
    wr.gate.OnTtsStop(E, 1);
    wr.Drain();
    EXPECT_EQ(wr.state, kDeviceStateIdle);
    wr.Take();
    wr.GwListenAuto();
    wr.Drain();
    EXPECT_EQ(wr.state, kDeviceStateListening);
    const auto listening = wr.Take();
    EXPECT_NE(std::find(listening.begin(), listening.end(), "drain"), listening.end());
    EXPECT_EQ(std::find(listening.begin(), listening.end(), "mic_on"), listening.end());  // still waits
    wr.TtsStart(2);
    wr.Drain();
    const auto out = wr.Take();
    EXPECT_EQ(wr.state, kDeviceStateSpeaking);
    EXPECT_EQ(std::find(out.begin(), out.end(), "mic_on"), out.end());
    EXPECT_EQ(std::find(out.begin(), out.end(), "popup"), out.end());
    EXPECT_NE(std::find(out.begin(), out.end(), "listen_stop"), out.end());
}

// The same wiring, the drain met by the old audio's write end before the new playback: the mic
// opens after the drain (the AutoStop wait ends normally).
TEST(PlaybackWiring, TheAutoStopWaitEndsWhenTheOldAudioIsWritten) {
    Wiring wr;
    ASSERT_EQ(wr.gate.Bind(E, "s"), g::Outcome::kBound);
    wr.Resync();
    ASSERT_TRUE(wr.gate.PostLinkUp(E));
    wr.TtsStart(1);
    ASSERT_TRUE(wr.ServerAudio());
    wr.gate.OnTtsStop(E, 1);
    wr.Drain();
    wr.GwListenAuto();
    wr.Drain();
    wr.Take();
    {
        std::unique_lock<std::mutex> lk(wr.audio_mu);
        auto item = wr.pipeline.TakeForDecode(lk);
        auto task = std::make_unique<AudioTask>();
        ASSERT_TRUE(wr.pipeline.Decoded(lk, item, std::move(task)));
        auto out = wr.pipeline.TakeForOutput(lk);
        ASSERT_TRUE(wr.pipeline.MarkWriteStart(lk, out));
        wr.pipeline.WriteEnd(lk, out.task->origin);
    }
    wr.Drain();
    const auto out = wr.Take();
    EXPECT_EQ(wr.state, kDeviceStateListening);
    EXPECT_NE(std::find(out.begin(), out.end(), "mic_on"), out.end());
    EXPECT_NE(std::find(out.begin(), out.end(), "popup"), out.end());
}

// A touch while speaking: R5 stops through the sink (the server audio is cleared), the abort and
// the listen start are queued together, the UI listens.
TEST(PlaybackWiring, ATouchWhileSpeakingClearsTheServerAudioAndListens) {
    Wiring wr;
    ASSERT_EQ(wr.gate.Bind(E, "s"), g::Outcome::kBound);
    wr.Resync();
    ASSERT_TRUE(wr.gate.PostLinkUp(E));
    wr.TtsStart(1);
    ASSERT_TRUE(wr.ServerAudio());
    wr.Drain();
    EXPECT_EQ(wr.state, kDeviceStateSpeaking);
    u::Event touch;
    touch.kind = u::EvKind::kTouch;
    wr.controller.Post(touch);
    wr.Drain();
    EXPECT_EQ(wr.state, kDeviceStateListening);
    std::unique_lock<std::mutex> lk(wr.audio_mu);
    EXPECT_TRUE(wr.pipeline.Empty(lk));
    EXPECT_EQ(wr.pipeline.book(lk).server_in_flight(), 0u);
    EXPECT_EQ(wr.gate.Snapshot().stop_serial, 1u);
    EXPECT_EQ(wr.pipeline.book(lk).stop_serial(), 1u);
    lk.unlock();
    auto first = wr.audio_q.Pop(0);  // the device abort and the listen start, in that order
    auto second = wr.audio_q.Pop(0);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first->payload.find("\"abort\""), std::string::npos);
    EXPECT_NE(second->payload.find("\"listen\""), std::string::npos);
    EXPECT_FALSE(wr.audio_q.Pop(0).has_value());
}

}  // namespace
