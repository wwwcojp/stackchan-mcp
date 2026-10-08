// StackChan FW-A2 §2.2, §2.4, §6.1, §7.1-2 (plan 1 handoff 3, 7, 11): enumerate interleavings
// of the gate core and the AudioService pipeline split as the real one is: the decode queue,
// the decoder's item, the playback queue, the output task's popped item, the item being written.
// Ops: tts start/stop, server/local audio, abort, touch, ClearForListening, death, pair end and
// the next bind, decode take/finish/failure, pop, mark, write end, and the AutoStop drain request.
// Checks: I1, the stop's clear decision matches the pipeline, played_after_stop matches a
// brute-force count (<= 1 per stop), the book's in-flight count matches the pipeline, and a
// drain notice comes only when no server audio is left and never stays pending without any.
#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "playback_book.h"
#include "playback_gate_core.h"

namespace g = stackchan::gate;
namespace a = stackchan::audio;

namespace {

struct Held {
    a::Origin origin;
    uint32_t gen = 0;  // the generation when the task took it
};

std::string OriginKey(const a::Origin& o) { return o.server ? "s" + std::to_string(o.accept_serial) : "l"; }

struct World {
    uint64_t e = 1;
    g::State gate;
    std::deque<a::Origin> decode_q;
    std::optional<Held> decoding;
    std::deque<a::Origin> play_q;
    std::optional<Held> popped;
    std::optional<a::Origin> writing;
    a::PlaybackBook book;
    std::vector<uint32_t> truth;  // truth[s]: frames counted for stop s
    bool requested = false;       // a drain request not answered yet
    uint32_t notices = 0;
    // abort, touch, death and pair end are all stops to the pipeline: they share one budget
    int starts = 2, pushes = 3, locals = 1, stops = 2, clears = 1, tts_stops = 1, decode_fails = 1, drains = 1;
    std::string trace;

    std::string Key() const {
        std::string k = std::to_string(e) + "|" + std::to_string(gate.current_gen) + "," +
                        std::to_string(gate.aborted_gen) + "," + std::to_string(gate.dev_abort_seq) + "," +
                        std::to_string(gate.accepting) + std::to_string(gate.speaking) +
                        std::to_string(gate.dead) + std::to_string(gate.bound_e) + "," +
                        std::to_string(gate.stop_serial) + "|q=";
        for (const auto& o : decode_q) k += OriginKey(o) + ",";
        k += "|d=" + (decoding ? OriginKey(decoding->origin) + "@" + std::to_string(book.generation() - decoding->gen) : "-");
        k += "|p=";
        for (const auto& o : play_q) k += OriginKey(o) + ",";
        k += "|o=" + (popped ? OriginKey(popped->origin) + "@" + std::to_string(book.generation() - popped->gen) : "-");
        k += "|w=" + (writing ? OriginKey(*writing) : "-");
        // the book's own state, or worlds that differ only there are merged
        k += "|pending=" + std::to_string(book.counter().pending()) + "|fly=" +
             std::to_string(book.server_in_flight()) + "|dp=" + std::to_string(book.drain_pending()) +
             "|serial=" + std::to_string(book.stop_serial()) + "|played=";
        for (uint32_t s = 1; s <= gate.stop_serial; s++) k += std::to_string(book.played().CountFor(s)) + ",";
        k += "|truth=";
        for (uint32_t t : truth) k += std::to_string(t) + ",";
        k += "|req=" + std::to_string(requested) + std::to_string(notices);
        for (int b : {starts, pushes, locals, stops, clears, tts_stops, decode_fails, drains}) {
            k += "," + std::to_string(b);
        }
        return k;
    }
};

uint32_t ServerInPipeline(const World& w) {
    uint32_t n = 0;
    for (const auto& o : w.decode_q) n += o.server;
    for (const auto& o : w.play_q) n += o.server;
    if (w.decoding) n += w.decoding->origin.server;
    if (w.popped) n += w.popped->origin.server;
    if (w.writing) n += w.writing->server;
    return n;
}

// server audio of the current serial that will still start writing (not yet marked, not doomed)
bool HasCurrentServer(const World& w) {
    auto cur = [&](const a::Origin& o) { return o.server && o.accept_serial == w.book.stop_serial(); };
    for (const auto& o : w.decode_q)
        if (cur(o)) return true;
    for (const auto& o : w.play_q)
        if (cur(o)) return true;
    if (w.decoding && w.decoding->gen == w.book.generation() && cur(w.decoding->origin)) return true;
    return w.popped && w.popped->gen == w.book.generation() && cur(w.popped->origin);
}

void Notice(World* w, bool met) {
    if (!met) return;
    EXPECT_TRUE(w->requested) << "a drain notice nobody asked for\n" << w->trace;
    EXPECT_EQ(ServerInPipeline(*w), 0u) << "drain notice while server audio is in flight\n" << w->trace;
    w->requested = false;
    w->notices++;
}

void Clear(World* w) {  // ResetDecoder(): empty the queues, bump the generation
    uint32_t removed = 0;
    for (const auto& o : w->decode_q) removed += o.server;
    for (const auto& o : w->play_q) removed += o.server;
    w->decode_q.clear();
    w->play_q.clear();
    Notice(w, w->book.OnCleared(removed));
}

void ApplyStop(World* w, const g::Result& r) {
    if (!r.stopped) return;
    const bool truth_clear = HasCurrentServer(*w);
    const bool clear = w->book.OnStop(r.state.stop_serial);
    ASSERT_EQ(clear, truth_clear) << "stop clear decision differs from the pipeline\n" << w->trace;
    if (clear) Clear(w);
    if (w->truth.size() <= r.state.stop_serial) w->truth.resize(r.state.stop_serial + 1, 0);
}

void Apply(World* w, const g::Result& r) {
    ApplyStop(w, r);
    w->gate = r.state;
}

using Op = bool (*)(World*);

bool Start(World* w) {
    if (w->starts == 0) return false;
    w->starts--;
    const g::Result r = g::OnTtsStart(w->gate, w->e, w->gate.current_gen + 1, w->gate.aborted_gen, w->gate.dev_abort_seq);
    Apply(w, r);
    if (r.outcome == g::Outcome::kAccepted && r.start_from_idle) Clear(w);  // §2.2 始める処理
    w->trace += " start";
    return true;
}
bool PushServer(World* w) {
    if (w->pushes == 0) return false;
    w->pushes--;
    if (g::OnServerAudio(w->gate, w->e).outcome == g::Outcome::kQueued) w->decode_q.push_back(w->book.OnServerQueued());
    w->trace += " push";
    return true;
}
bool PushLocal(World* w) {
    if (w->locals == 0) return false;
    w->locals--;
    w->decode_q.push_back(a::PlaybackBook::Local());
    w->trace += " local";
    return true;
}
bool Abort(World* w) {
    if (w->stops == 0 || w->gate.current_gen == 0) return false;
    w->stops--;
    Apply(w, g::OnAbort(w->gate, w->e, w->gate.current_gen));
    w->trace += " abort";
    return true;
}
bool Touch(World* w) {
    if (w->stops == 0) return false;
    w->stops--;
    Apply(w, g::OnTouch(w->gate, w->e));
    w->trace += " touch";
    return true;
}
bool TtsStop(World* w) {
    if (w->tts_stops == 0) return false;
    w->tts_stops--;
    Apply(w, g::OnTtsStop(w->gate, w->e, w->gate.current_gen));
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
bool Death(World* w) {  // F1 / F2 / a full queue: stop and dead
    if (w->stops == 0) return false;
    w->stops--;
    Apply(w, g::StopForDeath(w->gate, w->e));
    w->trace += " death";
    return true;
}
bool PairEnd(World* w) {  // K2: Unbind, then the next pair binds
    if (w->stops == 0) return false;
    w->stops--;
    Apply(w, g::Unbind(w->gate, w->e));
    w->e++;
    w->gate = g::Bind(w->gate, w->e).state;
    w->trace += " pair_end";
    return true;
}
bool DecodeTake(World* w) {
    if (w->decoding || w->decode_q.empty()) return false;
    w->decoding = Held{w->decode_q.front(), w->book.generation()};
    w->decode_q.pop_front();
    w->trace += " take";
    return true;
}
bool DecodeFinish(World* w) {  // the opus task's generation check, then to the playback queue
    if (!w->decoding) return false;
    const Held h = *w->decoding;
    w->decoding.reset();
    if (h.gen != w->book.generation()) {
        Notice(w, w->book.OnDecodeDropped(h.origin, h.gen));
        w->trace += " decode_stale";
    } else {
        w->play_q.push_back(h.origin);
        w->trace += " decoded";
    }
    return true;
}
bool DecodeFail(World* w) {  // decode failure (no decoder included)
    if (w->decode_fails == 0 || !w->decoding) return false;
    w->decode_fails--;
    const Held h = *w->decoding;
    w->decoding.reset();
    Notice(w, w->book.OnDecodeDropped(h.origin, h.gen));
    w->trace += " decode_fail";
    return true;
}
bool Pop(World* w) {
    if (w->popped || w->play_q.empty()) return false;
    w->popped = Held{w->play_q.front(), w->book.generation()};
    w->play_q.pop_front();
    w->trace += " pop";
    return true;
}
bool Mark(World* w) {
    if (!w->popped || w->writing) return false;
    const Held h = *w->popped;
    w->popped.reset();
    const a::PlaybackBook::Mark m = w->book.MarkWriteStart(h.origin, h.gen);
    Notice(w, m.drained);
    if (!m.write) {
        w->trace += " drop";
        return true;
    }
    EXPECT_FALSE(h.origin.server && h.origin.accept_serial < w->gate.stop_serial)
        << "I1: a frame accepted before a stop started writing after it\n" << w->trace;
    w->writing = h.origin;
    w->trace += " mark";
    return true;
}
bool WriteEnd(World* w) {
    if (!w->writing) return false;
    const a::Origin o = *w->writing;
    w->writing.reset();
    if (o.server) {
        for (uint32_t s = o.accept_serial + 1; s <= w->gate.stop_serial; s++) w->truth[s]++;
    }
    Notice(w, w->book.OnWriteEnd(o));
    w->trace += " end";
    return true;
}
bool Drain(World* w) {  // AutoStop asks (kRequestDrain)
    if (w->drains == 0 || w->requested) return false;
    w->drains--;
    w->requested = true;
    Notice(w, w->book.RequestDrain());
    w->trace += " drain";
    return true;
}

void Check(const World& w) {
    EXPECT_EQ(w.book.stop_serial(), w.gate.stop_serial) << "the book's serial is not the gate's\n" << w.trace;
    EXPECT_EQ(w.book.server_in_flight(), ServerInPipeline(w)) << "in-flight count\n" << w.trace;
    EXPECT_EQ(w.book.drain_pending(), w.requested) << "drain request state\n" << w.trace;
    if (w.requested) {
        EXPECT_GT(ServerInPipeline(w), 0u) << "a drain request left pending with nothing in flight\n" << w.trace;
    }
    for (uint32_t s = 1; s < w.truth.size(); s++) {
        EXPECT_EQ(w.book.played().CountFor(s), w.truth[s]) << "played_after_stop for stop " << s << "\n" << w.trace;
        EXPECT_LE(w.truth[s], 1u) << "more than one frame written after stop " << s << "\n" << w.trace;
    }
}

}  // namespace

TEST(PlaybackBook, EveryInterleavingOfTheSplitPipelineKeepsTheBook) {
    const std::vector<Op> ops = {Start, PushServer, PushLocal, Abort, Touch, TtsStop, ClearForListening,
                                 Death, PairEnd, DecodeTake, DecodeFinish, DecodeFail, Pop, Mark,
                                 WriteEnd, Drain};
    World init;
    init.gate = g::Bind(g::State{}, init.e).state;
    init.truth.assign(1, 0);
    std::vector<World> stack = {init};
    std::set<std::string> seen;
    size_t explored = 0, notices = 0, dead = 0, doomed = 0;
    while (!stack.empty()) {
        World w = stack.back();
        stack.pop_back();
        if (!seen.insert(w.Key()).second) continue;
        explored++;
        notices += w.notices > 0;
        dead += w.gate.dead;
        // an item the decoder or the output task holds from an older generation (dropped next)
        doomed += (w.decoding && w.decoding->gen != w.book.generation()) ||
                  (w.popped && w.popped->gen != w.book.generation());
        Check(w);
        if (::testing::Test::HasFailure()) return;
        for (Op op : ops) {
            World n = w;
            if (op(&n)) stack.push_back(n);
            if (::testing::Test::HasFailure()) return;
        }
    }
    EXPECT_GT(explored, 100000u);  // the enumeration really ran
    EXPECT_GT(notices, 1000u);
    EXPECT_GT(dead, 1000u);
    EXPECT_GT(doomed, 1000u);
}

TEST(PlaybackBook, DrainRightAwayWhenNoServerAudioIsInFlight) {
    a::PlaybackBook b;
    EXPECT_TRUE(b.RequestDrain());  // only local sounds or nothing: now
    EXPECT_FALSE(b.drain_pending());
    const a::Origin o = b.OnServerQueued();
    EXPECT_FALSE(b.RequestDrain());
    EXPECT_TRUE(b.drain_pending());
    const a::PlaybackBook::Mark m = b.MarkWriteStart(o, b.generation());
    EXPECT_TRUE(m.write);
    EXPECT_FALSE(m.drained);              // written, not yet drained (Codex review 143 I1)
    EXPECT_EQ(b.server_in_flight(), 1u);  // still being written: the mic would hear it
    EXPECT_TRUE(b.OnWriteEnd(o));         // met once
    EXPECT_FALSE(b.OnWriteEnd(b.Local()));
}

TEST(PlaybackBook, DroppingTheLastDoomedFrameAtTheMarkMeetsTheDrain) {
    a::PlaybackBook b;
    const a::Origin o = b.OnServerQueued();
    const uint32_t gen = b.generation();  // the output task took it at this generation
    EXPECT_FALSE(b.RequestDrain());
    b.OnCleared(0);  // a clear while it was held: doomed, nothing else removed
    const a::PlaybackBook::Mark m = b.MarkWriteStart(o, gen);
    EXPECT_FALSE(m.write);
    EXPECT_TRUE(m.drained);  // the last server frame left at the mark: tell AutoStop now
    EXPECT_EQ(b.server_in_flight(), 0u);
}

TEST(PlaybackBook, AQueueClearMeetsAPendingDrain) {
    a::PlaybackBook b;
    b.OnServerQueued();
    b.OnServerQueued();
    EXPECT_FALSE(b.RequestDrain());
    EXPECT_TRUE(b.OnStop(1));  // server audio of serial 0 not yet written: clear
    EXPECT_TRUE(b.OnCleared(2));
    EXPECT_EQ(b.generation(), 1u);
    EXPECT_EQ(b.server_in_flight(), 0u);
}

TEST(PlaybackBook, DecodeFailureUncountsOnlyTheCurrentGeneration) {
    a::PlaybackBook b;
    const a::Origin o1 = b.OnServerQueued();
    b.OnServerQueued();
    b.OnCleared(1);  // one was cleared, the other is being decoded at generation 0
    EXPECT_FALSE(b.OnDecodeDropped(o1, 0));
    EXPECT_EQ(b.server_in_flight(), 0u);
    const a::Origin o2 = b.OnServerQueued();
    EXPECT_FALSE(b.OnDecodeDropped(o2, b.generation()));
    EXPECT_EQ(b.counter().pending(), 0u);  // the current generation's failure uncounts it
    EXPECT_FALSE(b.OnStop(1));            // nothing left to clear: a local sound survives
}
