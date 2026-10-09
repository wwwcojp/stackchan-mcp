// StackChan FW-A2 plan 2B-2a (contract §5.1, design §6.1-6.2): the stat reply's groups and names,
// pure. The control receive task gathers the inputs (each from where it is safe to read: the gate,
// the pipeline, the UiController and the queues under their own locks, the hub's counters as
// atomics, the heap and the stacks from the ESP) and this turns them into the reply's groups.
// Every name the contract and the design list is here; a value that could not be read is 0.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "audio_pipeline.h"
#include "link_manager.h"
#include "playback_book.h"
#include "playback_gate.h"
#include "send_queue.h"
#include "ui_controller.h"
#include "wire.h"

namespace stackchan::link {

// A link side's counters, summed over the links of this boot (added when a link's task exits)
struct LinkTotals {
    uint32_t rx_frames = 0;
    uint32_t rx_dropped = 0;
    uint32_t bad_audio = 0;
    uint32_t extra_hellos = 0;
    uint32_t pongs_lost = 0;
    uint32_t tx_sent = 0;
    uint32_t tx_f2 = 0;
    uint32_t tx_errors = 0;
    uint32_t tx_stale = 0;
    uint32_t tx_bad_json = 0;
};

// The receive side's app ports' counts (AppLink::stats; Claude review 156 Minor 4)
struct AppStats {
    uint32_t mcp_unbound = 0;   // an mcp request with no bound pair, another pair or a dead one
    uint32_t unknown = 0;       // an unknown type, or a known one without its fields
    uint32_t stat_full = 0;     // a stat reply that did not fit the control queue (the pair ends)
    uint32_t stat_closed = 0;   // a stat reply for a pair that already ended
};

// The smallest free stack seen (bytes) per task kind; 0 when not measured yet
struct StackMarks {
    uint32_t audio_rx = 0;
    uint32_t ctrl_rx = 0;
    uint32_t audio_tx = 0;
    uint32_t ctrl_tx = 0;
    uint32_t link_mgr = 0;
    uint32_t link_conn = 0;
};

struct StatInputs {
    gate::State gate;
    gate::GateStats gate_stats;
    audio::PlaybackBook book;  // played_after_stop and the stop serial the AudioService copied
    audio::PipelineStats pipeline;
    ui::UiStats ui;
    std::array<uint32_t, kEndReasonCount> ends{};  // pairs ended, by link::EndReason (LinkManager::ends)
    uint32_t manager_stale = 0;                // LinkManager::stale_inputs (design §6.2: old E)
    uint32_t manager_duplicate_ends = 0;       // LinkManager::duplicate_ends
    uint32_t link_restarts = 0;                // NVS stackchan.link_restarts
    size_t notice_min_free = 0;
    net::QueueStats audio_queue;
    net::QueueStats ctrl_queue;
    LinkTotals audio_link;
    LinkTotals ctrl_link;
    AppStats app;
    StackMarks stacks;
    uint32_t heap_free = 0;
    uint32_t heap_min = 0;
    std::string build;  // the app version and the start of its ELF SHA-256
};

std::vector<wire::StatGroup> BuildStatGroups(const StatInputs& in);
// The reply for the control send queue: {"type":"stat","state":"done","req_id":ID,"build":...,<groups>}
std::string BuildStat(const std::string& req_id, const StatInputs& in);

}  // namespace stackchan::link
