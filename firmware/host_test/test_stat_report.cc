// StackChan FW-A2 plan 2B-2a (contract §5.1, design §6.1-6.2): the stat reply's groups and names.
// Every input gets its own value, and each must come out under its own group and name: the names
// are what 3E's DeviceLink and the probe read.
#include <gtest/gtest.h>

#include <cJSON.h>

#include <map>
#include <set>
#include <string>

#include "stat_report.h"

namespace a = stackchan::audio;
namespace l = stackchan::link;

namespace {

using Expected = std::map<std::string, uint64_t>;  // "group.name" -> value

l::StatInputs Fill(Expected* x) {
    l::StatInputs in;
    uint32_t v = 100;
    auto set = [&](uint32_t& field, const char* key) {
        field = ++v;
        (*x)[key] = field;
    };
    auto set_sz = [&](size_t& field, const char* key) {
        field = ++v;
        (*x)[key] = field;
    };
    in.gate.bound_e = (uint64_t{1} << 40) + 3;  // E above 2^32 (design §3.1) stays exact
    (*x)["pair.audio_epoch"] = in.gate.bound_e;
    set(in.gate.current_gen, "pair.current_gen");
    set(in.gate.aborted_gen, "pair.aborted_gen");
    set(in.gate.dev_abort_seq, "pair.dev_abort_seq");
    set(in.gate.stop_serial, "pair.stop_serial");
    auto& g = in.gate_stats;
    set(g.stops, "stops.total");
    set(g.stop_abort, "stops.abort");
    set(g.stop_tts_start, "stops.tts_start");
    set(g.stop_touch, "stops.touch");
    set(g.stop_f1, "stops.f1");
    set(g.stop_f2, "stops.f2");
    set(g.stop_closed, "stops.closed");
    set(g.stop_other_death, "stops.other_death");
    set(g.stop_violation, "stops.violation");
    set(g.stop_unbind, "stops.unbind");
    set(g.already, "stops.already");
    set(g.v_abort, "violations.abort");
    set(g.v_k_ahead, "violations.k_ahead");
    set(g.v_k_lowered, "violations.k_lowered");
    set(g.rejected_start, "rejected.tts_start");
    set(g.ignored_stop, "rejected.tts_stop");
    set(g.dropped_server, "rejected.server_audio");
    set(g.rejected_session, "rejected.abort_session");
    set(g.done_send_failed, "rejected.done_send_failed");
    set(g.stale, "ui.stale_gate");
    auto& p = in.pipeline;
    set(p.server_full, "audio.server_full");
    set(p.local_full, "audio.local_full");
    set(p.decode_failed, "audio.decode_failed");
    set(p.stale_decoded, "audio.stale_decoded");
    set(p.stale_output, "audio.stale_output");
    set(p.clears, "audio.clears");
    set(in.heap_free, "mem.heap_free");
    set(in.heap_min, "mem.heap_min");
    set(in.stacks.audio_rx, "stack.audio_rx");
    set(in.stacks.ctrl_rx, "stack.ctrl_rx");
    set(in.stacks.audio_tx, "stack.audio_tx");
    set(in.stacks.ctrl_tx, "stack.ctrl_tx");
    set(in.stacks.link_mgr, "stack.link_mgr");
    set(in.stacks.link_conn, "stack.link_conn");
    const std::pair<l::EndReason, const char*> ends[] = {
        {l::EndReason::kAudioClosed, "ends.audio_closed"},   {l::EndReason::kCtrlClosed, "ends.ctrl_closed"},
        {l::EndReason::kServerClose, "ends.server_close"},   {l::EndReason::kF1, "ends.f1"},
        {l::EndReason::kF2, "ends.f2"},                      {l::EndReason::kS6, "ends.s6"},
        {l::EndReason::kHelloTimeout, "ends.hello_timeout"}, {l::EndReason::kConnectFailed, "ends.connect_failed"},
        {l::EndReason::kViolation, "ends.violation"},        {l::EndReason::kQueueFull, "ends.queue_full"},
        {l::EndReason::kShutdown, "ends.shutdown"},
    };
    for (const auto& [reason, key] : ends) set(in.ends[static_cast<size_t>(reason)], key);
    in.ends[static_cast<size_t>(l::EndReason::kNone)] = 99999;  // never reported
    set(in.manager_stale, "ends.stale_inputs");
    set(in.manager_duplicate_ends, "ends.duplicate_ends");
    set(in.link_restarts, "ends.restarts");
    set_sz(in.notice_min_free, "ends.notice_min_free");
    set(in.audio_queue.mic_dropped, "queues.audio_mic_dropped");
    set(in.audio_queue.rejected_closed, "queues.audio_rejected_closed");
    set_sz(in.audio_queue.min_free_items, "queues.audio_min_free");
    set(in.ctrl_queue.rejected_closed, "queues.ctrl_rejected_closed");
    set_sz(in.ctrl_queue.min_free_items, "queues.ctrl_min_free");
    set(in.audio_link.tx_f2, "links.audio_f2");
    set(in.ctrl_link.tx_f2, "links.ctrl_f2");
    set(in.audio_link.tx_errors, "links.audio_send_errors");
    set(in.ctrl_link.tx_errors, "links.ctrl_send_errors");
    set(in.audio_link.tx_sent, "links.audio_sent");
    set(in.ctrl_link.tx_sent, "links.ctrl_sent");
    set(in.audio_link.rx_frames, "links.audio_rx_frames");
    set(in.ctrl_link.rx_frames, "links.ctrl_rx_frames");
    set(in.audio_link.bad_audio, "links.bad_audio");
    // the sums over both links
    in.audio_link.tx_stale = 7;
    in.ctrl_link.tx_stale = 11;
    (*x)["links.tx_stale"] = 18;
    in.audio_link.tx_bad_json = 37;
    in.ctrl_link.tx_bad_json = 41;
    (*x)["links.tx_bad_json"] = 78;
    in.audio_link.rx_dropped = 13;
    in.ctrl_link.rx_dropped = 17;
    (*x)["links.rx_dropped"] = 30;
    in.audio_link.extra_hellos = 19;
    in.ctrl_link.extra_hellos = 23;
    (*x)["links.extra_hellos"] = 42;
    in.audio_link.pongs_lost = 29;
    in.ctrl_link.pongs_lost = 31;
    (*x)["links.pongs_lost"] = 60;
    set(in.app.mcp_unbound, "app.mcp_unbound");
    set(in.app.unknown, "app.unknown");
    set(in.app.stat_full, "app.stat_full");
    set(in.app.stat_closed, "app.stat_closed");
    set(in.ui.stale, "ui.stale_ui");
    set(in.ui.dropped_input, "ui.dropped_input");
    set(in.ui.dropped_resync, "ui.dropped_resync");
    set(in.ui.refused_transitions, "ui.refused_transitions");
    set_sz(in.ui.max_depth, "ui.max_depth");
    // played_after_stop (design §6.1): a frame accepted before stop 1 and written after it, then
    // stop 2 with nothing after it (last 0, max 1)
    const a::Origin o = in.book.OnServerQueued();
    EXPECT_TRUE(in.book.MarkWriteStart(o, 0).write);
    in.book.OnStop(1);
    in.book.OnWriteEnd(o);
    in.book.OnStop(2);
    (*x)["played_after_stop.last"] = 0;
    (*x)["played_after_stop.max"] = 1;
    (*x)["played_after_stop.overflow"] = 0;
    (*x)["played_after_stop.stop_serial"] = 2;
    in.build = "2.2.6+sc.1 d0ef1ad7";
    return in;
}

}  // namespace

TEST(StatReport, EveryNameCarriesItsOwnValue) {
    Expected x;
    const l::StatInputs in = Fill(&x);
    const std::string text = l::BuildStat("r-1", in);
    cJSON* root = cJSON_Parse(text.c_str());
    ASSERT_NE(root, nullptr) << text;
    auto str = [&](const char* key) {
        const cJSON* v = cJSON_GetObjectItem(root, key);
        return cJSON_IsString(v) ? std::string(v->valuestring) : std::string("<missing>");
    };
    EXPECT_EQ(str("type"), "stat");
    EXPECT_EQ(str("state"), "done");
    EXPECT_EQ(str("req_id"), "r-1");
    EXPECT_EQ(str("build"), "2.2.6+sc.1 d0ef1ad7");
    Expected got;
    const cJSON* group = nullptr;
    cJSON_ArrayForEach(group, root) {
        if (!cJSON_IsObject(group)) continue;
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, group) {
            ASSERT_TRUE(cJSON_IsNumber(item)) << group->string << "." << item->string;
            const std::string key = std::string(group->string) + "." + item->string;
            EXPECT_EQ(got.count(key), 0u) << "twice: " << key;
            got[key] = static_cast<uint64_t>(item->valuedouble);
        }
    }
    cJSON_Delete(root);
    EXPECT_EQ(got, x);
}

TEST(StatReport, GroupsAreTheContractsAndNoBuildWhenEmpty) {
    Expected x;
    l::StatInputs in = Fill(&x);
    in.build.clear();
    std::set<std::string> groups;
    for (const auto& g : l::BuildStatGroups(in)) EXPECT_TRUE(groups.insert(g.name).second) << g.name;
    EXPECT_EQ(groups, (std::set<std::string>{"pair", "stops", "violations", "rejected", "played_after_stop", "audio",
                                             "mem", "stack", "ends", "queues", "links", "app", "ui"}));
    const std::string text = l::BuildStat("r-2", in);
    EXPECT_EQ(text.find("\"build\""), std::string::npos);
}
