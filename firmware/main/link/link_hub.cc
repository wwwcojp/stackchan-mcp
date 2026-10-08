// StackChan FW-A2 plan 2B-1: the link manager's ports on the ESP (see link_hub.h).
#include "link_hub.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "settings.h"
#include "wire.h"

#define TAG "LinkHub"

namespace stackchan::link {

namespace {

constexpr uint32_t kManagerStack = 4096;
constexpr UBaseType_t kManagerPriority = 6;  // design §1.1
constexpr uint32_t kWorkerStack = 4096;
constexpr UBaseType_t kWorkerPriority = 3;

std::string Print(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text != nullptr ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

const char* SideName(LinkSide side) { return side == LinkSide::kAudio ? "audio" : "ctrl"; }

}  // namespace

LinkHub::LinkHub(HubDeps deps) : d_(std::move(deps)), manager_(this, d_.notices), stamp_(d_.boot_count) {}

bool LinkHub::Start() {
    return xTaskCreate(ManagerMain, "link_mgr", kManagerStack, this, kManagerPriority, nullptr) == pdPASS;
}

void LinkHub::ManagerMain(void* arg) {
    LinkHub* hub = static_cast<LinkHub*>(arg);
    for (;;) {
        hub->manager_.RunOnce();
    }
}

void LinkHub::RequestShutdown() {
    Input in;
    in.kind = InKind::kShutdown;
    d_.notices->Post(in);
}

int64_t LinkHub::NowUs() { return esp_timer_get_time(); }

int64_t LinkHub::CtrlLastRxUs() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ctrl_ ? ctrl_->last_rx_us() : 0;
}

bool LinkHub::NextConnectionWraps() { return connected_once_ && stamp_.conn_index() == 65535; }

RxPorts LinkHub::RxPortsFor() {
    RxPorts p = d_.app;  // hello_reply, server_audio, app_json, stat (plan 2B-2)
    p.now_us = [] { return esp_timer_get_time(); };
    p.post = [n = d_.notices](const Input& in) { return n->Post(in); };
    gate::PlaybackGate* g = d_.gate;
    p.tts_start = [g](uint64_t e, const wire::TtsStart& t) { g->OnTtsStart(e, t); };
    p.tts_stop = [g](uint64_t e, uint32_t gen) { g->OnTtsStop(e, gen); };
    p.abort = [g](uint64_t e, const wire::AbortRequest& req) { g->OnAbort(e, req); };
    p.gw_listen = [ui = d_.ui](uint64_t e, const wire::GwListen& l) { ui->Post(ui::GwListenEvent(e, l)); };
    p.stop_for_death = [g](uint64_t e, EndReason r) { g->StopForDeath(e, r); };
    return p;
}

void LinkHub::StartWorker(Worker* w) {
    const char* name = w->side == LinkSide::kAudio ? "link_conn_a" : "link_conn_c";
    if (xTaskCreate(WorkerMain, name, kWorkerStack, w, kWorkerPriority, nullptr) == pdPASS) return;
    // No worker: report the failure and the worker's exit from here, as the worker would.
    ESP_LOGE(TAG, "%s worker did not start", SideName(w->side));
    Input result;
    result.kind = InKind::kConnectResult;
    result.attempt = w->attempt;
    result.ok = false;
    result.which = w->side == LinkSide::kAudio ? kAudioRx : kCtrlRx;
    result.e = w->e;
    d_.notices->Post(result);
    Input exited;
    exited.kind = InKind::kTaskExited;
    exited.attempt = w->attempt;
    exited.which = w->side == LinkSide::kAudio ? kAudioWorker : kCtrlWorker;
    d_.notices->Post(exited);
    delete w;
}

// Everything with a destructor lives here and is destroyed before WorkerMain posts the last
// notices: vTaskDelete never returns, so the task function's own locals would never be destroyed
// (Codex review 148 Important 1).
LinkHub::WorkerOutcome LinkHub::RunWorker(Worker* w) {
    LinkHub* hub = w->hub;
    const uint32_t attempt = w->attempt;
    WorkerOutcome out{};
    out.notices = hub->d_.notices;
    out.result.kind = InKind::kConnectResult;
    out.result.attempt = attempt;
    out.result.which = w->side == LinkSide::kAudio ? kAudioRx : kCtrlRx;
    out.result.e = w->e;
    out.end.kind = InKind::kEndRequest;  // an end seen by the link's tasks before the result
    out.end.e = w->e;
    out.end.reason = w->side == LinkSide::kAudio ? EndReason::kAudioClosed : EndReason::kCtrlClosed;
    out.exited.kind = InKind::kTaskExited;
    out.exited.attempt = attempt;
    out.exited.which = w->side == LinkSide::kAudio ? kAudioWorker : kCtrlWorker;
    {
        const ConnectInputs in = w->side == LinkSide::kAudio ? hub->d_.read_inputs() : w->inputs;
        const std::string url = w->side == LinkSide::kAudio ? AudioUrlFor(in, attempt) : w->url;
        const ConnectPlan plan = PlanConnect(in, url, w->side);
        std::unique_ptr<Link> link;
        if (!plan.ok) {
            ESP_LOGW(TAG, "%s attempt %u: %s (%s)", SideName(w->side), static_cast<unsigned>(attempt),
                     plan.error.c_str(), url.c_str());
        } else {
            LinkParams p;
            p.side = w->side;
            p.attempt = attempt;
            p.e = w->e;
            p.session_id = w->session_id;
            p.inputs = in;
            p.plan = plan;
            p.queue = w->side == LinkSide::kAudio ? hub->d_.audio_queue : hub->d_.ctrl_queue;
            p.notices = hub->d_.notices;
            p.rx_ports = hub->RxPortsFor();
            p.stop_for_death = [g = hub->d_.gate](uint64_t e, EndReason r) { g->StopForDeath(e, r); };
            link = Link::Connect(std::move(p), [hub, attempt] { return hub->stop_attempt_.load() == attempt; });
            if (link) {
                switch (link->Start()) {
                    case Link::StartResult::kStarted:
                        break;
                    case Link::StartResult::kFailed:
                        link.reset();
                        break;
                    case Link::StartResult::kStuck:
                        // The receive task runs but the send task did not start: never destroy
                        // its objects and never start another attempt beside it (design §3.4,
                        // §3.8). Restart.
                        link.release();
                        hub->Restart(w->e, "a link's send task did not start after its receive task");
                        break;
                }
            }
        }
        out.result.ok = link != nullptr;
        if (link) {
            out.link = link.get();  // alive until this worker's TaskExited: the manager waits for it
            hub->Adopt(w->side, attempt, std::move(link));
        }
    }
    delete w;
    return out;
}

void LinkHub::WorkerMain(void* arg) {
    const WorkerOutcome out = RunWorker(static_cast<Worker*>(arg));
    out.notices->Post(out.result);
    // An end the link's tasks saw before the result goes after it (design §3.8)
    if (out.link != nullptr && out.link->phase().OnResultPosted()) out.notices->Post(out.end);
    out.notices->Post(out.exited);  // the last touch of anything the manager may destroy
    vTaskDelete(nullptr);
}

void LinkHub::Adopt(LinkSide side, uint32_t attempt, std::unique_ptr<Link> link) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (links_attempt_ != attempt) {  // a new attempt: the old links were destroyed already
        audio_.reset();
        ctrl_.reset();
        links_attempt_ = attempt;
    }
    if (stop_attempt_.load() == attempt) link->RequestStop();  // ending already: stop at once
    (side == LinkSide::kAudio ? audio_ : ctrl_) = std::move(link);
}

// A hello or the ready: a full queue ends the pair (design §4.1, Codex review 148 Important 3)
void LinkHub::Push(net::SendQueue* q, uint64_t e, const std::string& json, const char* what) {
    gate::PlaybackGate* g = d_.gate;
    NoticeQueue* n = d_.notices;
    const net::PushResult r = QueueJsonOrEnd(
        *q, e, json, esp_timer_get_time(), [g](uint64_t pair, EndReason reason) { g->StopForDeath(pair, reason); },
        [n](const Input& in) { return n->Post(in); });
    if (r != net::PushResult::kQueued) ESP_LOGW(TAG, "%s not queued (%d)", what, static_cast<int>(r));
}

void LinkHub::ConnectAudio(uint32_t attempt) {
    stamp_.BeginConnection();  // the pair's E (design §3.1); NextConnectionWraps was checked first
    connected_once_ = true;
    StartWorker(new Worker{this, LinkSide::kAudio, attempt, stamp_.fw_epoch(), "", "", {}});
}

void LinkHub::SendAudioHello(uint32_t attempt, uint64_t e) {
    int version = 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (audio_ && links_attempt_ == attempt) version = audio_->protocol_version();
    }
    d_.audio_queue->Open(e);
    Push(d_.audio_queue, e, BuildAudioHello(version, d_.server_aec, d_.frame_duration_ms), "audio hello");
}

void LinkHub::ConnectCtrl(uint32_t attempt, uint64_t e) {
    std::string url, session;
    ConnectInputs inputs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (audio_) {
            url = audio_->url();
            session = audio_->rx().session_id();  // written before the hello reply was posted
            inputs = audio_->inputs();
        }
    }
    StartWorker(new Worker{this, LinkSide::kCtrl, attempt, e, url, session, inputs});
}

void LinkHub::SendCtrlHello(uint64_t e) {
    d_.ctrl_queue->Open(e);
    Push(d_.ctrl_queue, e, Print(wire::BuildCtrlHello(e)), "control hello");
}

void LinkHub::BindGate(uint64_t e) {
    std::string session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (audio_) session = audio_->rx().session_id();
    }
    d_.gate->Bind(e, session);
}

void LinkHub::SendReady(uint64_t e) { Push(d_.ctrl_queue, e, Print(wire::BuildReady(e)), "ready"); }

void LinkHub::PostLinkUp(uint64_t e) { d_.gate->PostLinkUp(e); }

void LinkHub::StopForDeath(uint64_t e, EndReason reason) { d_.gate->StopForDeath(e, reason); }

void LinkHub::UnbindGate(uint64_t e) { d_.gate->Unbind(e); }

void LinkHub::CloseQueues(uint64_t /*e*/, bool keep_ctrl) {
    d_.audio_queue->Close();
    if (keep_ctrl) {
        d_.ctrl_queue->CloseForFlush();
    } else {
        d_.ctrl_queue->Close();
    }
}

void LinkHub::PostLinkDown(uint64_t e) {
    ui::Event ev;
    ev.kind = ui::EvKind::kLinkDown;
    ev.e = e;
    d_.ui->Post(ev);
}

void LinkHub::RequestStop(uint64_t /*e*/) {
    const uint32_t attempt = manager_.state().attempt;
    stop_attempt_.store(attempt);  // the attempt's workers see it within a slice
    std::lock_guard<std::mutex> lock(mutex_);
    if (links_attempt_ != attempt) return;
    if (audio_) audio_->RequestStop();
    if (ctrl_) ctrl_->RequestStop();
}

void LinkHub::Destroy(uint64_t /*e*/) {
    std::lock_guard<std::mutex> lock(mutex_);  // every task of the attempt has exited
    audio_.reset();
    ctrl_.reset();
}

void LinkHub::Restart(uint64_t e, const char* why) {
    {
        Settings settings("stackchan", true);
        settings.SetInt("link_restarts", settings.GetInt("link_restarts") + 1);
    }  // the Settings destructor commits (design §3.5): leave its scope before restarting
    ESP_LOGE(TAG, "restart: %s (E %s)", why, std::to_string(e).c_str());  // 64-bit E: no 64-bit printf format (newlib nano)
    esp_restart();
}

}  // namespace stackchan::link
