// StackChan FW-A2 §7.1-2: enumerate interleavings of the gate core, the decode/playback
// queues (with origin marks) and the output task (pop, mark "write started", write end),
// and check I1 (no audio accepted before a stop starts writing after it), that a stop clears
// the queues only for server audio of the current serial (local sounds survive), and that
// played_after_stop matches a brute-force count and stays <= 1 per stop.
#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "audio_origin.h"
#include "playback_gate_core.h"

namespace g = stackchan::gate;
namespace a = stackchan::audio;

namespace {

constexpr uint32_t E = 1;

struct Item {
    a::Origin origin;
    bool operator<(const Item& o) const {
        return std::tie(origin.server, origin.accept_serial) <
               std::tie(o.origin.server, o.origin.accept_serial);
    }
    bool operator==(const Item& o) const {
        return origin.server == o.origin.server && origin.accept_serial == o.origin.accept_serial;
    }
};

struct World {
    g::State gate;
    std::deque<Item> queue;            // decode + playback queues (one FIFO for the model)
    std::optional<Item> popped;        // taken by the output task, not yet marked
    uint32_t popped_gen = 0;           // playback_generation_ when popped
    std::optional<Item> writing;       // marked "write started", OutputData() in progress
    uint32_t generation = 0;           // playback_generation_ (bumped by every clear)
    a::OriginCounter counter;
    a::PlayedAfterStop played;
    std::vector<uint32_t> truth;       // truth[s]: frames counted for stop s (index = serial)
    // budgets
    int starts = 2, pushes = 3, locals = 1, stops = 2, clears = 1, tts_stops = 1, decode_fails = 1;
    std::string trace;

    std::tuple<std::string, std::vector<Item>, bool, uint32_t, bool, uint32_t, uint32_t, int, int,
               int, int, int, int, int>
    Key() const {
        std::string gs = std::to_string(gate.current_gen) + "," + std::to_string(gate.aborted_gen) +
                         "," + std::to_string(gate.dev_abort_seq) + "," + std::to_string(gate.accepting) +
                         std::to_string(gate.speaking) + "," + std::to_string(gate.stop_serial);
        // the components' own state must be in the key, or worlds that differ only there are merged
        gs += "|pending=" + std::to_string(counter.pending()) + "|played=";
        for (uint32_t s = 1; s <= gate.stop_serial; s++) gs += std::to_string(played.CountFor(s)) + ",";
        gs += "|overflow=" + std::to_string(played.overflow()) + "|truth=";
        for (uint32_t t : truth) gs += std::to_string(t) + ",";
        std::vector<Item> q(queue.begin(), queue.end());
        return {gs,
                q,
                popped.has_value(),
                popped ? popped->origin.accept_serial * 2 + popped->origin.server : 0,
                writing.has_value(),
                writing ? writing->origin.accept_serial * 2 + writing->origin.server : 0,
                generation - popped_gen,
                starts,
                pushes,
                locals,
                stops,
                clears,
                tts_stops,
                decode_fails};
    }
};

bool HasCurrentServer(const World& w) {
    auto cur = [&](const Item& i) { return i.origin.server && i.origin.accept_serial == w.gate.stop_serial; };
    for (const auto& i : w.queue)
        if (cur(i)) return true;
    // a popped item whose generation moved is already doomed (dropped at the mark)
    return w.popped && w.popped_gen == w.generation && cur(*w.popped);
}

void Clear(World* w) {  // ResetDecoder(): empty the queues, bump the generation
    w->queue.clear();
    w->generation++;
    w->counter.OnCleared();
}

// Apply a gate result's stop: decide with the counter, then clear if needed.
void ApplyStop(World* w, const g::State& before, const g::Result& r) {
    if (!r.stopped) return;
    const bool truth_clear = HasCurrentServer(*w);
    w->gate = before;  // the counter is consulted before the serial moves
    const bool clear = w->counter.OnStop();
    ASSERT_EQ(clear, truth_clear) << "stop clear decision differs from the queue contents\n" << w->trace;
    if (clear) Clear(w);
    w->gate = r.state;
    w->played.OnStop(w->gate.stop_serial);
    if (w->truth.size() <= w->gate.stop_serial) w->truth.resize(w->gate.stop_serial + 1, 0);
}

using Step = bool (*)(World*);  // returns false when not applicable

bool Start(World* w) {
    if (w->starts == 0) return false;
    w->starts--;
    const uint32_t gen = w->gate.current_gen + 1;
    const g::Result r = g::OnTtsStart(w->gate, E, gen, w->gate.aborted_gen, w->gate.dev_abort_seq);
    const g::State before = w->gate;
    ApplyStop(w, before, r);
    w->gate = r.state;
    if (r.outcome == g::Outcome::kAccepted && r.start_from_idle) Clear(w);  // §2.2 始める処理
    w->trace += " start";
    return true;
}
bool PushServer(World* w) {
    if (w->pushes == 0) return false;
    w->pushes--;
    if (g::OnServerAudio(w->gate, E).outcome == g::Outcome::kQueued) {
        w->queue.push_back({{true, w->gate.stop_serial}});
        w->counter.OnServerQueued();
    }
    w->trace += " push";
    return true;
}
bool PushLocal(World* w) {
    if (w->locals == 0) return false;
    w->locals--;
    w->queue.push_back({{false, 0}});
    w->trace += " local";
    return true;
}
bool Abort(World* w) {
    if (w->stops == 0 || w->gate.current_gen == 0) return false;
    w->stops--;
    const g::State before = w->gate;
    const g::Result r = g::OnAbort(w->gate, E, w->gate.current_gen);
    ApplyStop(w, before, r);
    w->gate = r.state;
    w->trace += " abort";
    return true;
}
bool Touch(World* w) {
    if (w->stops == 0) return false;
    w->stops--;
    const g::State before = w->gate;
    const g::Result r = g::OnTouch(w->gate, E);
    ApplyStop(w, before, r);
    w->gate = r.state;
    w->trace += " touch";
    return true;
}
bool TtsStop(World* w) {
    if (w->tts_stops == 0) return false;
    w->tts_stops--;
    w->gate = g::OnTtsStop(w->gate, E, w->gate.current_gen).state;
    w->trace += " tts_stop";
    return true;
}
bool ClearForListening(World* w) {
    if (w->clears == 0) return false;
    w->clears--;
    if (g::ShouldClearForListening(w->gate)) Clear(w);
    w->trace += " clear_for_listening";
    return true;
}
bool DecodeFail(World* w) {  // the decoder drops the head item (current generation)
    if (w->decode_fails == 0 || w->queue.empty()) return false;
    w->decode_fails--;
    const Item it = w->queue.front();
    w->queue.pop_front();
    w->counter.OnDecodeDropped(it.origin, w->gate.stop_serial);
    w->trace += " decode_fail";
    return true;
}
bool Pop(World* w) {
    if (w->popped || w->queue.empty()) return false;
    w->popped = w->queue.front();
    w->queue.pop_front();
    w->popped_gen = w->generation;
    w->trace += " pop";
    return true;
}
bool Mark(World* w) {  // under audio_queue_mutex_: generation check + write-start mark
    if (!w->popped || w->writing) return false;
    const Item it = *w->popped;
    w->popped.reset();
    if (w->popped_gen != w->generation) {  // doomed by a clear: not reported to the counter
        w->trace += " drop";
        return true;
    }
    w->counter.OnMarked(it.origin, w->gate.stop_serial);
    // I1: audio accepted before the latest stop must not start writing after it
    EXPECT_FALSE(it.origin.server && it.origin.accept_serial < w->gate.stop_serial)
        << "I1: a frame accepted before a stop started writing after it\n" << w->trace;
    w->writing = it;
    w->trace += " mark";
    return true;
}
bool WriteEnd(World* w) {
    if (!w->writing) return false;
    const Item it = *w->writing;
    w->writing.reset();
    const uint32_t s_end = w->gate.stop_serial;
    w->played.OnWriteEnd(it.origin, s_end);
    if (it.origin.server)
        for (uint32_t s = it.origin.accept_serial + 1; s <= s_end; s++) w->truth[s]++;
    w->trace += " end";
    return true;
}

void CheckPlayed(const World& w) {
    for (uint32_t s = 1; s < w.truth.size(); s++) {
        EXPECT_EQ(w.played.CountFor(s), w.truth[s]) << "played_after_stop for stop " << s << "\n" << w.trace;
        EXPECT_LE(w.truth[s], 1u) << "more than one frame written after stop " << s << "\n" << w.trace;
    }
}

}  // namespace

TEST(AudioOrigin, EveryInterleavingKeepsI1AndCountsPlayedAfterStop) {
    const std::vector<Step> steps = {Start, PushServer, PushLocal, Abort, Touch,
                                     TtsStop, ClearForListening, Pop, Mark, WriteEnd, DecodeFail};
    World init;
    init.gate = g::Bind(g::State{}, E).state;
    init.truth.assign(1, 0);
    std::vector<World> stack = {init};
    std::set<decltype(init.Key())> seen;
    size_t explored = 0;
    while (!stack.empty()) {
        World w = stack.back();
        stack.pop_back();
        if (!seen.insert(w.Key()).second) continue;
        explored++;
        CheckPlayed(w);
        if (::testing::Test::HasFailure()) return;
        for (Step st : steps) {
            World n = w;
            if (st(&n)) stack.push_back(n);
            if (::testing::Test::HasFailure()) return;
        }
    }
    EXPECT_GT(explored, 1000u);  // the enumeration really ran
}

TEST(AudioOrigin, LocalSoundSurvivesAStopWithoutServerAudio) {
    a::OriginCounter c;
    EXPECT_FALSE(c.OnStop());  // only a local sound queued: no clear
    c.OnServerQueued();
    EXPECT_TRUE(c.OnStop());
    c.OnServerQueued();
    c.OnMarked({true, 2}, 2);  // marked: no longer pending
    EXPECT_FALSE(c.OnStop());
}

TEST(AudioOrigin, DecodeDropOfTheCurrentGenerationUncounts) {
    a::OriginCounter c;
    c.OnServerQueued();
    c.OnDecodeDropped({true, 0}, 0);  // the decoder dropped it: nothing left to clear
    EXPECT_EQ(c.pending(), 0u);
    EXPECT_FALSE(c.OnStop());
}

TEST(AudioOrigin, PlayedAfterStopCountsEveryCoveringStopAndOverflow) {
    a::PlayedAfterStop p;
    p.OnStop(1);
    p.OnStop(2);
    p.OnWriteEnd({true, 0}, 2);  // accepted at 0, written after stops 1 and 2
    EXPECT_EQ(p.CountFor(1), 1u);
    EXPECT_EQ(p.CountFor(2), 1u);
    EXPECT_EQ(p.last(), 1u);
    p.OnWriteEnd({false, 0}, 2);  // local sounds never count
    EXPECT_EQ(p.CountFor(2), 1u);
    for (uint32_t s = 3; s <= 12; s++) p.OnStop(s);
    p.OnWriteEnd({true, 1}, 12);  // stops 2..12: 2..4 are out of the 8-slot window
    EXPECT_EQ(p.overflow(), 3u);
    EXPECT_EQ(p.CountFor(12), 1u);
    EXPECT_EQ(p.CountFor(2), 0u);  // out of the window
    EXPECT_EQ(p.max(), 1u);
}
