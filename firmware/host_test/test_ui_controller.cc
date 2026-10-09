// StackChan FW-A2 §2.4 (v10): the UiController shell with fake ports (plan 1 handoff 1, 2, 5,
// 13; Codex reviews 140 Important 1 / 4, 141 Important 1).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ui_controller.h"

using namespace stackchan::ui;
namespace g = stackchan::gate;
namespace w = stackchan::wire;

namespace {

constexpr uint64_t E = 65536ull * 65536 + 5;

struct FakePorts : UiPorts {
    DeviceState state = kDeviceStateIdle;
    bool refuse_non_conversation = false;
    g::TouchResult touch{g::TouchOutcome::kNotSpeaking, 0};
    std::vector<std::string> log;
    void Add(const std::string& s) { log.push_back(s); }
    static std::string N(uint64_t v) { return std::to_string(v); }
    DeviceState CurrentState() override { return state; }
    bool SetDeviceState(DeviceState s) override {
        if (refuse_non_conversation && !IsConversation(s)) {
            Add("refused " + N(s));
            return false;
        }
        Add("state " + N(s));
        state = s;
        return true;
    }
    g::TouchResult GateTouch(uint64_t e, w::ListenMode m, w::DeviceAbortReason r) override {
        Add("gate " + N(e) + " " + N(static_cast<int>(m)) + " " + N(static_cast<int>(r)));
        return touch;
    }
    void ClearForListening() override { Add("clear"); }
    void SendListenStart(uint64_t e, Mode m) override { Add("listen_start " + N(e) + " " + N(static_cast<int>(m))); }
    void SendListenStop(uint64_t e) override { Add("listen_stop " + N(e)); }
    void SendWakeWord(uint64_t e) override { Add("wake_word " + N(e)); }
    void MicOn(Profile p) override { Add("mic_on " + N(static_cast<int>(p))); }
    void MicOff() override { Add("mic_off"); }
    void PlayPopup() override { Add("popup"); }
    void RequestDrain() override { Add("drain"); }
    void WakeDetect(bool on) override { Add(std::string("detect ") + (on ? "on" : "off")); }
    void ArmTimer(uint32_t req) override { Add("arm " + N(req)); }
    void CancelTimer() override { Add("cancel"); }
    void ShowWaitingForLink() override { Add("waiting"); }
    void StopMouth() override { Add("mouth_stop"); }
    void StartMouth() override { Add("mouth_start"); }
    void SetListeningLed(bool on) override { Add(std::string("led ") + (on ? "on" : "off")); }
    void SetPerformance(bool p) override { Add(std::string("power ") + (p ? "performance" : "low")); }
};

Event Ev(EvKind k, uint64_t e = 0) {
    Event ev;
    ev.kind = k;
    ev.e = e;
    return ev;
}

struct Rig {
    FakePorts ports;
    UiController ui;
    explicit Rig(UiConfig c = {}) : ui(&ports, c) {}
    void Feed(const Event& ev) {
        ui.Post(ev);
        ASSERT_TRUE(ui.ProcessOne());
    }
    // Idle and bound (the shell's Resync after the activation, then LinkUp)
    void Ready() {
        Feed(Ev(EvKind::kResync));
        Feed(Ev(EvKind::kLinkUp, E));
        ports.log.clear();
    }
    bool Logged(const std::string& s) const {
        for (const auto& x : ports.log)
            if (x == s) return true;
        return false;
    }
};

std::string S(DeviceState s) { return "state " + FakePorts::N(s); }

}  // namespace

TEST(UiShellPure, TouchRoutes) {
    EXPECT_EQ(ClassifyTouch(kDeviceStateStarting), TouchRoute::kEnterWifiConfig);
    EXPECT_EQ(ClassifyTouch(kDeviceStateWifiConfiguring), TouchRoute::kToggleAudioTesting);
    EXPECT_EQ(ClassifyTouch(kDeviceStateAudioTesting), TouchRoute::kToggleAudioTesting);
    for (DeviceState s : {kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking}) {
        EXPECT_EQ(ClassifyTouch(s), TouchRoute::kTouch);
    }
    for (DeviceState s : {kDeviceStateActivating, kDeviceStateUpgrading, kDeviceStateFatalError,
                          kDeviceStateConnecting, kDeviceStateUnknown}) {
        EXPECT_EQ(ClassifyTouch(s), TouchRoute::kDrop);
    }
    EXPECT_EQ(AudioTestingToggleTarget(kDeviceStateWifiConfiguring), kDeviceStateAudioTesting);
    EXPECT_EQ(AudioTestingToggleTarget(kDeviceStateAudioTesting), kDeviceStateWifiConfiguring);
    EXPECT_FALSE(AudioTestingToggleTarget(kDeviceStateIdle).has_value());       // moved on meanwhile
    EXPECT_FALSE(AudioTestingToggleTarget(kDeviceStateActivating).has_value());
}

TEST(UiShellPure, TouchRepliesAndModes) {
    EXPECT_EQ(ToTouchReply(g::TouchOutcome::kR5), TouchReply::kR5);
    EXPECT_EQ(ToTouchReply(g::TouchOutcome::kSendFailed), TouchReply::kSendFailed);
    EXPECT_EQ(ToTouchReply(g::TouchOutcome::kNotSpeaking), TouchReply::kNotSpeaking);
    EXPECT_EQ(ToTouchReply(g::TouchOutcome::kUnbound), TouchReply::kUnbound);
    EXPECT_EQ(ToWireMode(Mode::kManualStop), w::ListenMode::kManualStop);
    EXPECT_EQ(ToWireMode(Mode::kAutoStop), w::ListenMode::kAutoStop);
    EXPECT_EQ(ToWireMode(Mode::kRealtime), w::ListenMode::kRealtime);
}

TEST(UiShell, ATouchAsksTheGateOnceThenRunsTheActionsAndTheDerivedOnes) {
    Rig r;
    r.Ready();
    r.Feed(Ev(EvKind::kTouch));
    EXPECT_EQ(r.ports.log, (std::vector<std::string>{
                               "gate " + FakePorts::N(E) + " 0 0", S(kDeviceStateListening),
                               "listen_start " + FakePorts::N(E) + " 0", "clear", "mic_on 0", "popup", "arm 1",
                               "detect off", "led on", "power performance"}));
    EXPECT_EQ(r.ui.state().disp, Disp::kListening);
    EXPECT_EQ(r.ui.stats().gate_calls, 1u);
}

TEST(UiShell, AWakeWordAsksWithItsModeAndReasonAndAToggleLikeATouch) {
    Rig r;
    r.Ready();
    Event wake = Ev(EvKind::kWakeWord);
    wake.mode = Mode::kAutoStop;
    r.Feed(wake);
    EXPECT_EQ(r.ports.log.front(), "gate " + FakePorts::N(E) + " 1 1");
    r.ports.log.clear();
    r.Feed(Ev(EvKind::kToggle));
    EXPECT_EQ(r.ports.log.front(), "gate " + FakePorts::N(E) + " 0 0");
}

TEST(UiShell, ATouchWithoutAPairIsUnboundWhateverTheGateSays) {
    Rig r;
    r.Feed(Ev(EvKind::kResync));
    r.ports.log.clear();
    r.ports.touch = {g::TouchOutcome::kR5, 9};
    r.Feed(Ev(EvKind::kTouch));
    EXPECT_TRUE(r.Logged("waiting"));
    EXPECT_FALSE(r.Logged(S(kDeviceStateListening)));
}

TEST(UiShell, TheWakeWordAudioIsNotSentInThisBuild) {
    Rig off;
    off.Ready();
    off.Feed(Ev(EvKind::kWakeWord));
    EXPECT_FALSE(off.Logged("wake_word " + FakePorts::N(E)));
    Rig on(UiConfig{false, false, true});
    on.Ready();
    on.Feed(Ev(EvKind::kWakeWord));
    EXPECT_TRUE(on.Logged("wake_word " + FakePorts::N(E)));
}

TEST(UiShell, ASuspendedTouchNeverCallsTheGate) {  // plan 1 handoff 1
    Rig r;
    r.Ready();
    r.ui.EnterNonConversation(kDeviceStateWifiConfiguring);
    r.ports.log.clear();
    r.ports.touch = {g::TouchOutcome::kR5, 3};
    r.Feed(Ev(EvKind::kTouch));
    EXPECT_TRUE(r.ports.log.empty());
    EXPECT_EQ(r.ui.stats().gate_calls, 0u);
}

TEST(UiShell, AnOldResyncDoesNotResumeBehindANonConversationState) {  // Codex review 141 I1
    Rig r;
    r.Ready();
    r.Feed(Ev(EvKind::kTouch));  // Listening
    r.ports.state = kDeviceStateIdle;  // an error made the outside Idle and posted a Resync
    r.ui.Post(Ev(EvKind::kResync));
    r.ports.log.clear();
    r.ui.EnterNonConversation(kDeviceStateWifiConfiguring);  // before that Resync is processed
    EXPECT_EQ(r.ports.log, (std::vector<std::string>{"listen_stop " + FakePorts::N(E), "mic_off", "cancel",
                                                     "led off", S(kDeviceStateWifiConfiguring)}));
    r.ports.log.clear();
    ASSERT_TRUE(r.ui.ProcessOne());  // the old Resync: dropped
    EXPECT_EQ(r.ui.stats().dropped_resync, 1u);
    EXPECT_TRUE(r.ui.state().suspended);
    Event gw = Ev(EvKind::kGwListenStart, E);
    r.Feed(gw);
    EXPECT_FALSE(r.Logged("mic_on 0"));  // the gateway cannot open the mic behind the settings
    EXPECT_TRUE(r.ports.log.empty());
}

TEST(UiShell, LeavingFromListeningOrSpeakingGoesThroughIdle) {
    Rig r;
    r.Ready();
    r.Feed(Ev(EvKind::kTouch));
    r.ports.log.clear();
    r.ui.EnterNonConversation(kDeviceStateUpgrading);
    EXPECT_EQ(r.ports.log, (std::vector<std::string>{"listen_stop " + FakePorts::N(E), "mic_off", "cancel", "led off",
                                                     S(kDeviceStateIdle), S(kDeviceStateUpgrading)}));
}

TEST(UiShell, ARefusedTransitionGoesBackToTheConversation) {
    Rig r;
    r.Ready();
    r.ports.refuse_non_conversation = true;
    r.ui.EnterNonConversation(kDeviceStateWifiConfiguring);
    EXPECT_TRUE(r.ui.state().suspended);
    EXPECT_EQ(r.ui.stats().refused_transitions, 1u);
    ASSERT_TRUE(r.ui.ProcessOne());  // the Resync it posted
    EXPECT_FALSE(r.ui.state().suspended);
    EXPECT_EQ(r.ui.state().disp, Disp::kIdle);
}

TEST(UiShell, TheLedFollowsListening) {
    Rig r;
    r.Ready();
    r.Feed(Ev(EvKind::kTouch));
    EXPECT_TRUE(r.Logged("led on"));
    r.ports.log.clear();
    r.Feed(Ev(EvKind::kGwListenStop, E));
    EXPECT_TRUE(r.Logged("led off"));
    r.ports.log.clear();
    r.Feed(Ev(EvKind::kGwListenStop, E));  // not listening: nothing
    EXPECT_FALSE(r.Logged("led off"));
}

TEST(UiShell, TheMouthStartsWhenSpeakingStartsAndTheDetectorIsTheShells) {
    Rig r(UiConfig{false, true, false});
    r.Ready();
    Event start = Ev(EvKind::kGateChanged, E);
    start.spk = true;
    start.rev = 1;
    r.Feed(start);
    EXPECT_TRUE(r.Logged(S(kDeviceStateSpeaking)));
    EXPECT_TRUE(r.Logged("mouth_start"));
    EXPECT_TRUE(r.Logged("detect on"));
    r.ports.log.clear();
    Event again = start;
    again.rev = 2;  // a newer rev, still speaking: no new start
    r.Feed(again);
    EXPECT_FALSE(r.Logged("mouth_start"));
}

TEST(UiShell, AWakeWordThatChangedNothingWhileSpeakingReArmsTheDetector) {
    Rig r(UiConfig{false, true, false});
    r.Ready();
    Event start = Ev(EvKind::kGateChanged, E);
    start.spk = true;
    start.rev = 1;
    r.Feed(start);
    r.ports.log.clear();
    r.ports.touch = {g::TouchOutcome::kSendFailed, 2};
    r.Feed(Ev(EvKind::kWakeWord));
    EXPECT_EQ(r.ui.state().disp, Disp::kSpeaking);
    EXPECT_TRUE(r.Logged("detect on"));
}

TEST(UiShell, PerformanceOnLinkUpAndOnTheChannelOpenLowPowerOnItsClose) {
    Rig r;
    r.Feed(Ev(EvKind::kResync));
    r.ports.log.clear();
    r.Feed(Ev(EvKind::kLinkUp, E));  // the link came back without listening (Codex review 140 I1)
    EXPECT_EQ(r.ports.log, (std::vector<std::string>{"power performance"}));
    r.ports.log.clear();
    r.Feed(Ev(EvKind::kTouch));  // the channel opens
    EXPECT_TRUE(r.Logged("power performance"));
    r.ports.log.clear();
    r.Feed(Ev(EvKind::kToggle));  // Toggle while listening closes the channel
    EXPECT_TRUE(r.Logged("power low"));
}

TEST(UiShell, PostWakesTheMainTaskAndTracksTheDepth) {
    Rig r;
    int wakes = 0;
    r.ui.SetWake([&] { wakes++; });
    r.ui.Post(Ev(EvKind::kResync));
    r.ui.Post(Ev(EvKind::kLinkUp, E));
    EXPECT_EQ(wakes, 2);
    EXPECT_EQ(r.ui.stats().max_depth, 2u);
    EXPECT_TRUE(r.ui.ProcessOne());
    EXPECT_TRUE(r.ui.ProcessOne());
    EXPECT_FALSE(r.ui.ProcessOne());
}

TEST(UiShell, TheGatesRevFromATouchMakesAnOlderGateChangedStale) {
    Rig r;
    r.Ready();
    r.ports.touch = {g::TouchOutcome::kNotSpeaking, 7};
    r.Feed(Ev(EvKind::kTouch));
    EXPECT_EQ(r.ui.state().grev, 7u);
    Event old = Ev(EvKind::kGateChanged, E);
    old.spk = true;
    old.rev = 5;  // sent before the touch's answer: already known
    r.Feed(old);
    EXPECT_EQ(r.ui.state().disp, Disp::kListening);
    EXPECT_EQ(r.ui.stats().stale, 1u);
}

TEST(UiShell, TheListeningDetectorSettingReachesStep) {
    Rig r(UiConfig{true, false, false});
    r.Ready();
    r.Feed(Ev(EvKind::kTouch));
    EXPECT_TRUE(r.Logged("detect on"));
    EXPECT_FALSE(r.Logged("detect off"));
}

// The gateway's listen as a UiController event (the audio receive task posts it, design §2.4)
TEST(UiControllerShell, TheGatewaysListenAsAnEvent) {
    constexpr uint64_t kE = 65536ull * 65536 + 9;
    w::GwListen l;
    l.start = true;
    l.mode = w::ListenMode::kAutoStop;
    l.profile = w::ListenProfile::kRaw;
    Event ev = GwListenEvent(kE, l);
    EXPECT_EQ(ev.kind, EvKind::kGwListenStart);
    EXPECT_EQ(ev.e, kE);
    EXPECT_EQ(ev.mode, Mode::kAutoStop);
    EXPECT_EQ(ev.profile, Profile::kRaw);
    l.mode = w::ListenMode::kRealtime;
    l.profile = w::ListenProfile::kVoice;
    ev = GwListenEvent(kE, l);
    EXPECT_EQ(ev.mode, Mode::kRealtime);
    EXPECT_EQ(ev.profile, Profile::kVoice);
    l.mode = w::ListenMode::kManualStop;
    EXPECT_EQ(GwListenEvent(kE, l).mode, Mode::kManualStop);
    l.start = false;
    ev = GwListenEvent(kE, l);
    EXPECT_EQ(ev.kind, EvKind::kGwListenStop);
    EXPECT_EQ(ev.e, kE);
}

// stat (plan 2A follow-up 6): a user input while the conversation is suspended is dropped by Step
// and counted by the shell; the gateway's listen is not a user input.
TEST(UiShell, AUserInputWhileSuspendedIsCounted) {
    Rig r;  // suspended until the first Resync (Starting, Activating)
    r.Feed(Ev(EvKind::kTouch));
    r.Feed(Ev(EvKind::kWakeWord));
    r.Feed(Ev(EvKind::kToggle));
    r.Feed(Ev(EvKind::kGwListenStart, E));
    r.Feed(Ev(EvKind::kPlaybackDrained));
    EXPECT_EQ(r.ui.stats().dropped_input, 3u);
    r.Ready();
    r.Feed(Ev(EvKind::kTouch));
    EXPECT_EQ(r.ui.stats().dropped_input, 3u);
    r.ui.EnterNonConversation(kDeviceStateWifiConfiguring);
    r.Feed(Ev(EvKind::kToggle));
    EXPECT_EQ(r.ui.stats().dropped_input, 4u);
}
