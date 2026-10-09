// StackChan FW-A2 plan 2B-2a: the stat reply's groups (see stat_report.h).
#include "stat_report.h"

#include <cJSON.h>

namespace stackchan::link {

namespace {

uint32_t End(const StatInputs& in, EndReason r) { return in.ends[static_cast<size_t>(r)]; }

}  // namespace

std::vector<wire::StatGroup> BuildStatGroups(const StatInputs& in) {
    const gate::GateStats& g = in.gate_stats;
    std::vector<wire::StatGroup> out;
    // contract §5.1: the generations
    out.push_back({"pair",
                   {{"audio_epoch", in.gate.bound_e},
                    {"current_gen", in.gate.current_gen},
                    {"aborted_gen", in.gate.aborted_gen},
                    {"dev_abort_seq", in.gate.dev_abort_seq},
                    {"stop_serial", in.gate.stop_serial}}});
    // contract §5.1: the stop processing by trigger, and `already`
    out.push_back({"stops",
                   {{"total", g.stops},
                    {"abort", g.stop_abort},
                    {"tts_start", g.stop_tts_start},
                    {"touch", g.stop_touch},
                    {"f1", g.stop_f1},
                    {"f2", g.stop_f2},
                    {"closed", g.stop_closed},
                    {"other_death", g.stop_other_death},
                    {"violation", g.stop_violation},
                    {"unbind", g.stop_unbind},
                    {"already", g.already}}});
    // contract §5.1 (R1.2), design §6.2 (the K lowered), R2.2
    out.push_back({"violations", {{"abort", g.v_abort}, {"k_ahead", g.v_k_ahead}, {"k_lowered", g.v_k_lowered}}});
    out.push_back({"rejected",
                   {{"tts_start", g.rejected_start},
                    {"tts_stop", g.ignored_stop},
                    {"server_audio", g.dropped_server},
                    {"abort_session", g.rejected_session},
                    {"done_send_failed", g.done_send_failed}}});
    // contract §5.1, design §6.1
    out.push_back({"played_after_stop",
                   {{"last", in.book.played().last()},
                    {"max", in.book.played().max()},
                    {"overflow", in.book.played().overflow()},
                    {"stop_serial", in.book.stop_serial()}}});
    out.push_back({"audio",
                   {{"server_full", in.pipeline.server_full},
                    {"local_full", in.pipeline.local_full},
                    {"decode_failed", in.pipeline.decode_failed},
                    {"stale_decoded", in.pipeline.stale_decoded},
                    {"stale_output", in.pipeline.stale_output},
                    {"clears", in.pipeline.clears}}});
    // contract §5.1: the heap and the stacks
    out.push_back({"mem", {{"heap_free", in.heap_free}, {"heap_min", in.heap_min}}});
    out.push_back({"stack",
                   {{"audio_rx", in.stacks.audio_rx},
                    {"ctrl_rx", in.stacks.ctrl_rx},
                    {"audio_tx", in.stacks.audio_tx},
                    {"ctrl_tx", in.stacks.ctrl_tx},
                    {"link_mgr", in.stacks.link_mgr},
                    {"link_conn", in.stacks.link_conn}}});
    // contract §5.1: the pairs ended by trigger; design §3.8, §6.2: restarts, the notice queue
    out.push_back({"ends",
                   {{"audio_closed", End(in, EndReason::kAudioClosed)},
                    {"ctrl_closed", End(in, EndReason::kCtrlClosed)},
                    {"server_close", End(in, EndReason::kServerClose)},
                    {"f1", End(in, EndReason::kF1)},
                    {"f2", End(in, EndReason::kF2)},
                    {"s6", End(in, EndReason::kS6)},
                    {"hello_timeout", End(in, EndReason::kHelloTimeout)},
                    {"connect_failed", End(in, EndReason::kConnectFailed)},
                    {"violation", End(in, EndReason::kViolation)},
                    {"queue_full", End(in, EndReason::kQueueFull)},
                    {"shutdown", End(in, EndReason::kShutdown)},
                    {"stale_inputs", in.manager_stale},
                    {"duplicate_ends", in.manager_duplicate_ends},
                    {"restarts", in.link_restarts},
                    {"notice_min_free", in.notice_min_free}}});
    // design §6.2: the mic audio dropped, the queues' room
    out.push_back({"queues",
                   {{"audio_mic_dropped", in.audio_queue.mic_dropped},
                    {"audio_rejected_closed", in.audio_queue.rejected_closed},
                    {"audio_min_free", in.audio_queue.min_free_items},
                    {"ctrl_rejected_closed", in.ctrl_queue.rejected_closed},
                    {"ctrl_min_free", in.ctrl_queue.min_free_items}}});
    // design §6.2: F2 per link
    out.push_back({"links",
                   {{"audio_f2", in.audio_link.tx_f2},
                    {"ctrl_f2", in.ctrl_link.tx_f2},
                    {"audio_send_errors", in.audio_link.tx_errors},
                    {"ctrl_send_errors", in.ctrl_link.tx_errors},
                    {"audio_sent", in.audio_link.tx_sent},
                    {"ctrl_sent", in.ctrl_link.tx_sent},
                    {"tx_stale", in.audio_link.tx_stale + in.ctrl_link.tx_stale},
                    {"tx_bad_json", in.audio_link.tx_bad_json + in.ctrl_link.tx_bad_json},
                    {"audio_rx_frames", in.audio_link.rx_frames},
                    {"ctrl_rx_frames", in.ctrl_link.rx_frames},
                    {"rx_dropped", in.audio_link.rx_dropped + in.ctrl_link.rx_dropped},
                    {"bad_audio", in.audio_link.bad_audio},
                    {"extra_hellos", in.audio_link.extra_hellos + in.ctrl_link.extra_hellos},
                    {"pongs_lost", in.audio_link.pongs_lost + in.ctrl_link.pongs_lost}}});
    // the receive side's app ports (Claude review 156 Minor 4)
    out.push_back({"app",
                   {{"mcp_unbound", in.app.mcp_unbound},
                    {"unknown", in.app.unknown},
                    {"stat_full", in.app.stat_full},
                    {"stat_closed", in.app.stat_closed},
                    {"out_unbound", in.app.out_unbound},
                    {"out_closed", in.app.out_closed},
                    {"out_bad_json", in.app.out_bad_json}}});
    // design §2.2, §2.4, §6.2: what came for another pair, or while not in a conversation
    out.push_back({"ui",
                   {{"stale_gate", g.stale},
                    {"stale_ui", in.ui.stale},
                    {"dropped_input", in.ui.dropped_input},
                    {"dropped_resync", in.ui.dropped_resync},
                    {"refused_transitions", in.ui.refused_transitions},
                    {"max_depth", in.ui.max_depth}}});
    return out;
}

std::string BuildStat(const std::string& req_id, const StatInputs& in) {
    cJSON* root = wire::BuildStatReply(req_id, BuildStatGroups(in), in.build);
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text != nullptr ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

}  // namespace stackchan::link
