// StackChan FW-A2 §2.4: UiController Step, one test per row of the rule table.
#include <gtest/gtest.h>

#include <functional>
#include <vector>

#include "ui_controller_core.h"

using namespace stackchan::ui;

namespace {

Event Ev(EvKind k) {
    Event e;
    e.kind = k;
    return e;
}
Event Touch(TouchReply r, uint32_t rev = 0) {
    Event e = Ev(EvKind::kTouch);
    e.reply = r;
    e.rev = rev;
    return e;
}
Event Wake(TouchReply r, Mode m = Mode::kAutoStop, uint32_t rev = 0) {
    Event e = Ev(EvKind::kWakeWord);
    e.reply = r;
    e.mode = m;
    e.rev = rev;
    return e;
}
Event Toggle(TouchReply r) {
    Event e = Ev(EvKind::kToggle);
    e.reply = r;
    return e;
}
Event Gate(uint32_t pair, bool spk, uint32_t rev) {
    Event e = Ev(EvKind::kGateChanged);
    e.e = pair;
    e.spk = spk;
    e.rev = rev;
    return e;
}
Event LinkUp(uint32_t pair, bool spk = false, uint32_t rev = 0) {
    Event e = Ev(EvKind::kLinkUp);
    e.e = pair;
    e.spk = spk;
    e.rev = rev;
    return e;
}
Event LinkDown(uint32_t pair) {
    Event e = Ev(EvKind::kLinkDown);
    e.e = pair;
    return e;
}
Event GwStart(uint32_t pair, Mode m = Mode::kManualStop, Profile p = Profile::kVoice) {
    Event e = Ev(EvKind::kGwListenStart);
    e.e = pair;
    e.mode = m;
    e.profile = p;
    return e;
}
Event GwStop(uint32_t pair) {
    Event e = Ev(EvKind::kGwListenStop);
    e.e = pair;
    return e;
}
Event Timeout(uint32_t req) {
    Event e = Ev(EvKind::kListenTimeout);
    e.req = req;
    return e;
}

State RunAll(State s, const std::vector<Event>& evs) {
    for (const auto& e : evs) s = Step(s, e).state;
    return s;
}
State Ready(uint32_t pair = 1) { return RunAll(State{}, {Ev(EvKind::kResync), LinkUp(pair)}); }
State Listening(uint32_t pair = 1) { return RunAll(Ready(pair), {Touch(TouchReply::kNotSpeaking)}); }
State Speaking(uint32_t pair = 1) { return RunAll(Ready(pair), {Gate(pair, true, 1)}); }

using A = std::vector<Action>;

// the K151 build: no wake word detection while listening (wake_in_listening off)
A StartManual(uint32_t e, uint32_t req, Profile p = Profile::kVoice, Mode m = Mode::kManualStop) {
    return {SetDisplay(Disp::kListening), SendListenStart(e, m), Simple(ActKind::kClearForListening),
            MicOn(p), Simple(ActKind::kPlayPopup), ArmTimer(req), WakeDetect(false)};
}
A Stop(uint32_t e) {
    return {SendListenStop(e), Simple(ActKind::kMicOff), Simple(ActKind::kCancelTimer),
            SetDisplay(Disp::kIdle), WakeDetect(true)};
}

}  // namespace

TEST(UiControllerCore, StartsSuspendedAndIgnoresUserInputUntilResync) {
    StepResult r = Step(State{}, Touch(TouchReply::kNotSpeaking));
    EXPECT_TRUE(r.actions.empty());
    EXPECT_EQ(r.state.disp, Disp::kOther);
    r = Step(r.state, LinkUp(1));  // connection events still update knowledge
    EXPECT_EQ(r.state.e, 1u);
    EXPECT_TRUE(r.actions.empty());
    r = Step(r.state, Ev(EvKind::kResync));
    EXPECT_EQ(r.state.disp, Disp::kIdle);
    EXPECT_FALSE(r.state.suspended);
    EXPECT_EQ(r.actions, A({SetDisplay(Disp::kIdle), WakeDetect(true)}));
}

TEST(UiControllerCore, ResyncWhileTheGateSpeaksShowsSpeaking) {
    State s = RunAll(State{}, {LinkUp(1, true, 3)});
    StepResult r = Step(s, Ev(EvKind::kResync));
    EXPECT_EQ(r.state.disp, Disp::kSpeaking);
    EXPECT_EQ(r.actions, A({SetDisplay(Disp::kSpeaking)}));
}

TEST(UiControllerCore, LinkUpCarriesTheGateState) {
    State s = RunAll(State{}, {Ev(EvKind::kResync)});
    StepResult r = Step(s, LinkUp(2, true, 5));
    EXPECT_EQ(r.state.e, 2u);
    EXPECT_TRUE(r.state.gspk);
    EXPECT_EQ(r.state.grev, 5u);
    EXPECT_EQ(r.state.disp, Disp::kSpeaking);
    // a GateChanged queued before the LinkUp (rev <= 5) is stale afterwards
    EXPECT_TRUE(Step(r.state, Gate(2, false, 5)).stale);
}

TEST(UiControllerCore, LinkDownEndsListeningWithoutListenStop) {
    State s = Listening(1);
    StepResult r = Step(s, LinkDown(1));
    EXPECT_EQ(r.state.e, 0u);
    EXPECT_EQ(r.state.chan_e, 0u);
    EXPECT_FALSE(r.state.gspk);
    EXPECT_EQ(r.state.disp, Disp::kIdle);
    EXPECT_EQ(r.actions, A({Simple(ActKind::kMicOff), Simple(ActKind::kCancelTimer), SetDisplay(Disp::kIdle),
                            WakeDetect(true)}));
    EXPECT_TRUE(Step(s, LinkDown(2)).stale);
}

TEST(UiControllerCore, GateChangedSpeakingPreemptsListeningAndSendsListenStop) {
    State s = Listening(1);
    StepResult r = Step(s, Gate(1, true, 1));
    EXPECT_EQ(r.state.disp, Disp::kSpeaking);
    EXPECT_FALSE(r.state.mic);
    EXPECT_EQ(r.actions, A({SendListenStop(1), Simple(ActKind::kMicOff), Simple(ActKind::kCancelTimer),
                            SetDisplay(Disp::kSpeaking)}));
    r = Step(r.state, Gate(1, false, 2));
    EXPECT_EQ(r.state.disp, Disp::kIdle);
    EXPECT_EQ(r.actions, A({Simple(ActKind::kStopMouth), SetDisplay(Disp::kIdle), WakeDetect(true)}));
}

TEST(UiControllerCore, GateChangedStaleByPairOrRev) {
    State s = Speaking(1);  // grev 1
    EXPECT_TRUE(Step(s, Gate(2, false, 9)).stale);
    EXPECT_TRUE(Step(s, Gate(1, false, 1)).stale);
    EXPECT_FALSE(Step(s, Gate(1, false, 2)).stale);
}

TEST(UiControllerCore, TouchR5ListensOnTheCurrentPairAndTheGateSentListenStart) {
    State s = Speaking(1);
    StepResult r = Step(s, Touch(TouchReply::kR5, 2));
    EXPECT_EQ(r.state.disp, Disp::kListening);
    EXPECT_EQ(r.state.chan_e, 1u);
    EXPECT_EQ(r.state.req, s.req + 1);
    EXPECT_FALSE(r.state.gspk);
    EXPECT_EQ(r.state.grev, 2u);
    // leaving the Speaking display always stops the mouth: the later GateChanged(false) is stale
    EXPECT_EQ(r.actions, A({Simple(ActKind::kStopMouth), SetDisplay(Disp::kListening), MicOn(Profile::kVoice),
                            Simple(ActKind::kPlayPopup), ArmTimer(r.state.req), WakeDetect(false)}));
}

TEST(UiControllerCore, TouchR5WhileTheDisplayIsListeningOnlyPlaysThePopup) {
    State s = Listening(1);
    StepResult r = Step(s, Touch(TouchReply::kR5, 3));
    EXPECT_EQ(r.state.disp, Disp::kListening);
    EXPECT_EQ(r.actions, A({Simple(ActKind::kPlayPopup), ArmTimer(r.state.req), WakeDetect(false)}));
}

TEST(UiControllerCore, TouchSendFailedDoesNothingAndUnboundShowsWaiting) {
    State s = Speaking(1);
    StepResult r = Step(s, Touch(TouchReply::kSendFailed));
    EXPECT_TRUE(r.actions.empty());
    EXPECT_EQ(r.state.disp, Disp::kSpeaking);
    r = Step(Ready(1), Touch(TouchReply::kUnbound));
    EXPECT_EQ(r.state.disp, Disp::kIdle);
    EXPECT_EQ(r.actions, A({Simple(ActKind::kShowWaitingForLink), WakeDetect(true)}));
}

TEST(UiControllerCore, TouchNotSpeakingStartsOrStops) {
    StepResult r = Step(Ready(1), Touch(TouchReply::kNotSpeaking));
    EXPECT_EQ(r.state.disp, Disp::kListening);
    EXPECT_EQ(r.state.chan_e, 1u);
    EXPECT_EQ(r.actions, StartManual(1, r.state.req));
    r = Step(r.state, Touch(TouchReply::kNotSpeaking));
    EXPECT_EQ(r.state.disp, Disp::kIdle);
    EXPECT_EQ(r.actions, Stop(1));
}

TEST(UiControllerCore, TouchNotSpeakingWhileTheDisplayStillSaysSpeakingStarts) {
    State s = Speaking(1);  // GateChanged(false) not processed yet
    StepResult r = Step(s, Touch(TouchReply::kNotSpeaking, 2));
    EXPECT_FALSE(r.state.gspk);
    EXPECT_EQ(r.state.grev, 2u);
    EXPECT_EQ(r.state.disp, Disp::kListening);
    A want = {Simple(ActKind::kStopMouth)};
    for (const auto& a : StartManual(1, r.state.req)) want.push_back(a);
    EXPECT_EQ(r.actions, want);
    EXPECT_TRUE(Step(r.state, Gate(1, false, 2)).stale);  // the stop notice is older
}

TEST(UiControllerCore, WakeWordStartsAutoStopAndWaitsForTheDrain) {
    StepResult r = Step(Ready(1), Wake(TouchReply::kNotSpeaking, Mode::kAutoStop));
    EXPECT_EQ(r.state.disp, Disp::kListening);
    EXPECT_TRUE(r.state.wait);
    EXPECT_FALSE(r.state.mic);
    EXPECT_EQ(r.actions, A({SetDisplay(Disp::kListening), SendListenStart(1, Mode::kAutoStop), SendWakeWord(1),
                            Simple(ActKind::kRequestDrain), ArmTimer(r.state.req), WakeDetect(false)}));
    r = Step(r.state, Ev(EvKind::kPlaybackDrained));
    EXPECT_TRUE(r.state.mic);
    EXPECT_FALSE(r.state.wait);
    EXPECT_EQ(r.actions, A({MicOn(Profile::kVoice), Simple(ActKind::kPlayPopup)}));
    EXPECT_TRUE(Step(r.state, Ev(EvKind::kPlaybackDrained)).actions.empty());  // not waiting now
}

TEST(UiControllerCore, WakeWordWhileListeningResendsListenStart) {
    State s = Listening(1);
    StepResult r = Step(s, Wake(TouchReply::kNotSpeaking));
    EXPECT_EQ(r.state.disp, Disp::kListening);
    // a wake word detected in Idle and queued behind the touch that started listening
    EXPECT_EQ(r.actions, A({SendListenStart(1, Mode::kManualStop), Simple(ActKind::kPlayPopup), WakeDetect(false)}));
}

TEST(UiControllerCore, WakeWordDuringRawListeningIsDroppedAndDetectionStops) {
    State s = Step(Ready(1), GwStart(1, Mode::kManualStop, Profile::kRaw)).state;
    ASSERT_EQ(s.profile, Profile::kRaw);
    StepResult r = Step(s, Wake(TouchReply::kNotSpeaking));
    EXPECT_EQ(r.state.disp, Disp::kListening);
    EXPECT_EQ(r.actions, A({WakeDetect(false)}));
}

TEST(UiControllerCore, ToggleStopsAndClosesTheChannel) {
    StepResult r = Step(Listening(1), Toggle(TouchReply::kNotSpeaking));
    EXPECT_EQ(r.state.disp, Disp::kIdle);
    EXPECT_EQ(r.state.chan_e, 0u);
    EXPECT_EQ(r.actions, Stop(1));
}

TEST(UiControllerCore, GatewayListenStartAndStop) {
    StepResult r = Step(Ready(1), GwStart(1, Mode::kManualStop, Profile::kRaw));
    EXPECT_EQ(r.state.disp, Disp::kListening);
    EXPECT_EQ(r.actions, StartManual(1, r.state.req, Profile::kRaw));
    EXPECT_TRUE(Step(r.state, GwStop(2)).stale);
    r = Step(r.state, GwStop(1));
    EXPECT_EQ(r.state.disp, Disp::kIdle);
    EXPECT_EQ(r.state.profile, Profile::kVoice);  // ListeningProfileAfterStop
    EXPECT_EQ(r.actions, Stop(1));
    // while the gate speaks, the gateway's listen start is dropped (not stale)
    r = Step(Speaking(1), GwStart(1));
    EXPECT_FALSE(r.stale);
    EXPECT_TRUE(r.actions.empty());
    EXPECT_EQ(r.state.disp, Disp::kSpeaking);
}

TEST(UiControllerCore, ListenTimeoutOnlyForTheCurrentRequest) {
    State s = Listening(1);
    EXPECT_TRUE(Step(s, Timeout(s.req + 7)).stale);
    StepResult r = Step(s, Timeout(s.req));
    EXPECT_EQ(r.state.disp, Disp::kIdle);
    EXPECT_EQ(r.actions, Stop(1));
}

TEST(UiControllerCore, SuspendStopsListeningAndDropsUserInput) {
    StepResult r = Step(Listening(1), Ev(EvKind::kSuspend));
    EXPECT_TRUE(r.state.suspended);
    EXPECT_EQ(r.state.disp, Disp::kOther);
    EXPECT_EQ(r.actions, A({SendListenStop(1), Simple(ActKind::kMicOff), Simple(ActKind::kCancelTimer)}));
    StepResult t = Step(r.state, Touch(TouchReply::kNotSpeaking));
    EXPECT_TRUE(t.actions.empty());
    t = Step(r.state, LinkDown(1));  // knowledge still updated, no display effects
    EXPECT_EQ(t.state.e, 0u);
    EXPECT_TRUE(t.actions.empty());
}

TEST(UiControllerCore, R5WhileWaitingForTheDrainOpensTheMic) {
    // Codex review 131 Important 2: AutoStop listening (mic closed, waiting) then R5
    State s = Step(Ready(1), GwStart(1, Mode::kAutoStop)).state;
    ASSERT_TRUE(s.wait);
    ASSERT_FALSE(s.mic);
    StepResult r = Step(s, Touch(TouchReply::kR5, 2));
    EXPECT_TRUE(r.state.mic);
    EXPECT_FALSE(r.state.wait);
    EXPECT_EQ(r.actions, A({MicOn(Profile::kVoice), Simple(ActKind::kPlayPopup), ArmTimer(r.state.req),
                            WakeDetect(false)}));
}

TEST(UiControllerCore, R5DuringRawListeningSwitchesToTheVoiceMic) {
    State s = Step(Ready(1), GwStart(1, Mode::kManualStop, Profile::kRaw)).state;
    StepResult r = Step(s, Touch(TouchReply::kR5, 2));
    EXPECT_EQ(r.state.profile, Profile::kVoice);
    EXPECT_EQ(r.actions, A({MicOn(Profile::kVoice), Simple(ActKind::kPlayPopup), ArmTimer(r.state.req),
                            WakeDetect(false)}));
}

TEST(UiControllerCore, ResyncAfterAnErrorWhileListeningStopsListening) {
    // Codex review 131 Important 3: MAIN_EVENT_ERROR -> Idle -> Resync from a conversation state
    StepResult r = Step(Listening(1), Ev(EvKind::kResync));
    EXPECT_EQ(r.state.disp, Disp::kIdle);
    EXPECT_FALSE(r.state.mic);
    EXPECT_EQ(r.state.timer_req, 0u);
    EXPECT_EQ(r.actions, Stop(1));
}

TEST(UiControllerCore, ResyncAfterAnErrorWhileSpeakingShowsSpeakingAgain) {
    // Codex review 132 Important 2: the error made the real display Idle; the state still says
    // Speaking, so Resync must show it again rather than do nothing
    StepResult r = Step(Speaking(1), Ev(EvKind::kResync));
    EXPECT_EQ(r.state.disp, Disp::kSpeaking);
    EXPECT_EQ(r.actions, A({SetDisplay(Disp::kSpeaking)}));
    r = Step(Ready(1), Ev(EvKind::kResync));  // an error from Idle: show Idle again
    EXPECT_EQ(r.actions, A({SetDisplay(Disp::kIdle), WakeDetect(true)}));
}

TEST(UiControllerCore, ResyncWhileListeningAndTheGateSpeaksEndsListeningAndShowsSpeaking) {
    // the gate started speaking but its GateChanged is still in the queue behind the error
    State s = Listening(1);
    s.gspk = true;
    StepResult r = Step(s, Ev(EvKind::kResync));
    EXPECT_EQ(r.state.disp, Disp::kSpeaking);
    EXPECT_FALSE(r.state.mic);
    EXPECT_EQ(r.actions, A({SendListenStop(1), Simple(ActKind::kMicOff), Simple(ActKind::kCancelTimer),
                            SetDisplay(Disp::kSpeaking)}));
}

TEST(UiControllerCore, R5SetsTheDetectorForListeningPerTheSetting) {
    // Codex review 132 Important 3, Claude review 133 Important 1: after R5 the detector is
    // set for voice listening: off on the K151 build, on when the setting says so
    StepResult r = Step(Speaking(1), Wake(TouchReply::kR5, Mode::kAutoStop, 3));
    EXPECT_EQ(r.actions.back(), WakeDetect(false));
    State on = Speaking(1);
    on.wake_in_listening = true;
    EXPECT_EQ(Step(on, Wake(TouchReply::kR5, Mode::kAutoStop, 3)).actions.back(), WakeDetect(true));
    State raw = Step(Ready(1), GwStart(1, Mode::kManualStop, Profile::kRaw)).state;
    raw.wake_in_listening = true;
    EXPECT_EQ(Step(raw, Touch(TouchReply::kR5, 2)).actions.back(), WakeDetect(true));
}

TEST(UiControllerCore, ListeningKeepsTheDetectorOffUnlessTheSettingSaysSo) {
    EXPECT_EQ(Step(Ready(1), Touch(TouchReply::kNotSpeaking)).actions.back(), WakeDetect(false));
    State on = Ready(1);
    on.wake_in_listening = true;
    EXPECT_EQ(Step(on, Touch(TouchReply::kNotSpeaking)).actions.back(), WakeDetect(true));
    // raw listening never wants it
    EXPECT_EQ(Step(on, GwStart(1, Mode::kManualStop, Profile::kRaw)).actions.back(), WakeDetect(false));
}

TEST(UiControllerCore, WakeWordThatChangesNothingRestoresTheDetectorWhereTheDisplayWantsIt) {
    EXPECT_EQ(Step(Ready(1), Wake(TouchReply::kSendFailed)).actions, A({WakeDetect(true)}));
    EXPECT_TRUE(Step(Speaking(1), Wake(TouchReply::kSendFailed)).actions.empty());
    EXPECT_TRUE(Step(Ready(1), Touch(TouchReply::kSendFailed)).actions.empty());
    // unbound while the UI still knows a pair (LinkDown not processed yet): no detector on raw
    State raw = Step(Ready(1), GwStart(1, Mode::kManualStop, Profile::kRaw)).state;
    EXPECT_EQ(Step(raw, Touch(TouchReply::kUnbound)).actions, A({Simple(ActKind::kShowWaitingForLink)}));
}

TEST(UiControllerCore, SuspendResetsTheProfileLikeStopping) {
    // Claude review 133 Minor 5: raw listening -> Suspend -> Resync leaves no raw profile behind
    State raw = Step(Ready(1), GwStart(1, Mode::kManualStop, Profile::kRaw)).state;
    State s = Step(raw, Ev(EvKind::kSuspend)).state;
    EXPECT_EQ(s.profile, Profile::kVoice);
    EXPECT_EQ(Step(s, Ev(EvKind::kResync)).state.profile, Profile::kVoice);
}

TEST(UiControllerCore, TheGateIsNotAskedWhileSuspendedOrForARawWakeWord) {
    State s = Step(Speaking(1), Ev(EvKind::kSuspend)).state;
    EXPECT_FALSE(NeedsGateDecision(s, Touch(TouchReply::kNotSpeaking)));
    EXPECT_TRUE(NeedsGateDecision(Ready(1), Touch(TouchReply::kNotSpeaking)));
    EXPECT_TRUE(NeedsGateDecision(Ready(1), Toggle(TouchReply::kNotSpeaking)));
    State raw = Step(Ready(1), GwStart(1, Mode::kManualStop, Profile::kRaw)).state;
    EXPECT_FALSE(NeedsGateDecision(raw, Wake(TouchReply::kNotSpeaking)));
    EXPECT_TRUE(NeedsGateDecision(raw, Touch(TouchReply::kNotSpeaking)));
    EXPECT_FALSE(NeedsGateDecision(Ready(1), GwStart(1)));
}

// Design §7.1-4: every event sequence up to a depth, checking that the effects and the state
// agree (mic, timer, the real display, the wake word detector) and the state invariants hold
// after every step.
TEST(UiControllerCore, EveryShortSequenceKeepsEffectsAndStateInStep) {
    struct World {
        State s;
        bool mic = false;             // from MicOn / MicOff
        uint32_t timer = 0;           // from ArmTimer / CancelTimer
        Disp screen = Disp::kOther;   // from SetDisplay; others own it outside the conversation
        bool wake = false;            // from WakeDetect; the detector stops itself when it fires
        int fired = 0;                // detected wake words waiting in the queue (up to 2)
    };
    auto events = [](const World& w) {
        // events as the shell and the other tasks can produce them: the gate answers OnTouch(e)
        // with "unbound" when it has no pair, also while the UI still knows one (LinkDown not
        // processed yet); the manager posts LinkUp only after LinkDown; a wake word is in the
        // queue only after the detector fired, and other events may be processed before it
        const State& s = w.s;
        std::vector<Event> v;
        std::vector<TouchReply> replies = {TouchReply::kUnbound};
        if (s.e != 0) replies = {TouchReply::kR5, TouchReply::kSendFailed, TouchReply::kNotSpeaking,
                                 TouchReply::kUnbound};
        for (TouchReply r : replies) {
            v.push_back(Touch(r, s.grev + 1));
            if (w.fired > 0) v.push_back(Wake(r, Mode::kAutoStop, s.grev + 1));
            v.push_back(Toggle(r));
        }
        const uint32_t e = s.e != 0 ? s.e : 1;
        v.push_back(Gate(e, true, s.grev + 1));
        v.push_back(Gate(e, false, s.grev + 1));
        if (s.e == 0) {
            v.push_back(LinkUp(s.chan_e + 1, false, s.grev));
            v.push_back(LinkUp(s.chan_e + 1, true, s.grev + 1));
        } else {
            v.push_back(LinkDown(s.e));
        }
        v.push_back(GwStart(e, Mode::kAutoStop, Profile::kRaw));
        v.push_back(GwStart(e, Mode::kManualStop));
        v.push_back(GwStop(e));
        v.push_back(Timeout(s.req));
        v.push_back(Ev(EvKind::kPlaybackDrained));
        v.push_back(Ev(EvKind::kResync));
        v.push_back(Ev(EvKind::kSuspend));
        return v;
    };
    size_t steps = 0;
    std::function<void(const World&, int)> walk = [&](const World& w, int depth) {
        if (depth == 0 || ::testing::Test::HasFailure()) return;
        // the detector fires: it stops itself, the event is queued. Besides the detector Step
        // turned on, the shell's detector may fire while Speaking, and any while suspended
        // (Claude review 133 Minor 3)
        // a second detection needs the detector on again, e.g. Resync -> fire -> Resync
        // (Codex review 134 Minor 2)
        if (w.fired < 2 && (w.wake || w.s.disp == Disp::kSpeaking || w.s.suspended)) {
            World n = w;
            n.wake = false;
            n.fired++;
            walk(n, depth - 1);
        }
        for (const Event& ev : events(w)) {
            World n = w;
            if (ev.kind == EvKind::kWakeWord) n.fired--;
            if (ev.kind == EvKind::kResync || ev.kind == EvKind::kSuspend) {
                n.screen = Disp::kOther;  // an error or another state drew it; unknown to the UI
                n.wake = false;
            }
            const StepResult r = Step(w.s, ev);
            n.s = r.state;
            for (const Action& a : r.actions) {
                if (a.kind == ActKind::kMicOn) n.mic = true;
                if (a.kind == ActKind::kMicOff) n.mic = false;
                if (a.kind == ActKind::kArmTimer) n.timer = a.req;
                if (a.kind == ActKind::kCancelTimer) n.timer = 0;
                if (a.kind == ActKind::kSetDisplay) n.screen = a.disp;
                if (a.kind == ActKind::kWakeDetect) n.wake = a.on;
            }
            steps++;
            const int kind = static_cast<int>(ev.kind);
            ASSERT_EQ(n.mic, n.s.mic) << "mic effects and state disagree (event kind " << kind << ")";
            ASSERT_EQ(n.timer, n.s.timer_req) << "timer effects and state disagree";
            if (!n.s.suspended) {
                ASSERT_EQ(n.screen, n.s.disp) << "the real display and the state disagree (event kind " << kind << ")";
                // the detector is on where the display wants it, unless a detection still waits in
                // the queue (its Step turns the detector back on), and off while listening where
                // the display does not want it (Claude review 133 Important 1)
                const bool listening_wants = n.s.wake_in_listening && n.s.profile == Profile::kVoice;
                if (n.s.disp == Disp::kIdle || (n.s.disp == Disp::kListening && listening_wants)) {
                    ASSERT_TRUE(n.wake || n.fired > 0) << "the detector is off (event kind " << kind << ")";
                }
                if (n.s.disp == Disp::kListening && !listening_wants) {
                    ASSERT_FALSE(n.wake) << "the detector is on while listening (event kind " << kind << ")";
                }
                if (n.s.disp == Disp::kSpeaking) {  // Resync relies on it (Claude review 133 Minor 2)
                    ASSERT_TRUE(n.s.gspk && n.s.e != 0) << "Speaking without the gate speaking";
                }
            }
            if (n.s.disp == Disp::kListening) {
                ASSERT_NE(n.s.e, 0u) << "listening without a pair";
                ASSERT_EQ(n.s.chan_e, n.s.e) << "listening on another pair's channel";
            }
            if (!n.s.suspended && n.s.gspk) ASSERT_EQ(n.s.disp, Disp::kSpeaking);
            if (n.s.wait) ASSERT_TRUE(n.s.disp == Disp::kListening && !n.s.mic);
            walk(n, depth - 1);
        }
    };
    for (bool setting : {false, true}) {  // the K151 build, and CONFIG_..._IN_LISTENING with AFE
        World w0;
        w0.s.wake_in_listening = setting;
        walk(w0, 6);
    }
    EXPECT_GT(steps, 1000000u);
}
