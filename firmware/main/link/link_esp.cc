// StackChan FW-A2 plan 2B-1: one link on the ESP (see link_esp.h).
#include "link_esp.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include "ws_frame.h"

#define TAG "Link"

namespace stackchan::link {

namespace {

const char* SideName(LinkSide side) { return side == LinkSide::kAudio ? "audio" : "ctrl"; }

uint32_t RxBit(LinkSide side) { return side == LinkSide::kAudio ? kAudioRx : kCtrlRx; }
uint32_t TxBit(LinkSide side) { return side == LinkSide::kAudio ? kAudioTx : kCtrlTx; }

Input Exited(uint32_t attempt, uint32_t bit) {
    Input in;
    in.kind = InKind::kTaskExited;
    in.attempt = attempt;
    in.which = bit;
    return in;
}

RxPorts WithLinkPorts(RxPorts p, std::atomic<int64_t>* last_rx, net::SendQueue* queue,
                      const std::function<void(uint64_t, EndReason)>& stop_for_death) {
    p.received = [last_rx](int64_t now) { last_rx->store(now); };
    p.stop_for_death = stop_for_death;
    p.push_pong = [queue](uint64_t e, std::string payload, int64_t now) {
        return queue->Push(e, net::ElemKind::kPong, std::move(payload), now);
    };
    return p;
}

}  // namespace

LinkTasks TasksFor(LinkSide side) {
    if (side == LinkSide::kAudio) {
        // The audio receive task runs the gate, cJSON and MCP parsing: more stack than today's 4096
        // (design §9; stat shows the high-water mark for plan 3).
        return LinkTasks{EspTcpTaskOptions{"audio_rx", 1, 6144}, "audio_tx", 3, 4096};
    }
    return LinkTasks{EspTcpTaskOptions{"ctrl_rx", 5, 4096}, "ctrl_tx", 5, 4096};
}

Link::Link(LinkParams params)
    : p_(std::move(params)),
      rx_(p_.side, p_.attempt, p_.e, p_.session_id, p_.plan.version, &phase_,
          WithLinkPorts(p_.rx_ports, &last_rx_us_, p_.queue, p_.stop_for_death)),
      tx_(p_.side, p_.queue, &phase_,
          TxPorts{[] { return esp_timer_get_time(); },
                  [this](const uint8_t* d, size_t n, int64_t timeout) { return tcp_.SendSlice(d, n, timeout); },
                  [this] { return Stopping(); },
                  [](uint8_t m[4]) {
                      const uint32_t r = esp_random();
                      for (int i = 0; i < 4; i++) m[i] = static_cast<uint8_t>(r >> (8 * i));
                  },
                  p_.stop_for_death, [n = p_.notices](const Input& in) { return n->Post(in); },
                  [](int64_t us) { vTaskDelay(pdMS_TO_TICKS(us / 1000) > 0 ? pdMS_TO_TICKS(us / 1000) : 1); }}) {}

Link::~Link() = default;  // the manager destroys a link only after its tasks exited

std::unique_ptr<Link> Link::Connect(LinkParams params, const std::function<bool()>& stop) {
    std::unique_ptr<Link> link(new Link(std::move(params)));
    const WsTarget& t = link->p_.plan.target;
    if (!link->tcp_.ConnectManaged(t.host, t.port, kTcpConnectMs, stop)) return nullptr;
    if (!link->Handshake(stop)) return nullptr;  // no task was started: destroying it is safe
    return link;
}

bool Link::Handshake(const std::function<bool()>& stop) {
    const int64_t deadline = esp_timer_get_time() + int64_t{kHandshakeMs} * 1000;
    uint8_t key[16];
    esp_fill_random(key, sizeof(key));
    const WsTarget& t = p_.plan.target;
    const std::string request = wsframe::BuildHandshake(t.host, t.port, t.path, p_.plan.headers,
                                                        wsframe::Base64(key, sizeof(key)));
    const net::SendOutcome sent = net::SendAll(
        reinterpret_cast<const uint8_t*>(request.data()), request.size(), deadline, [] { return esp_timer_get_time(); },
        [this](const uint8_t* d, size_t n, int64_t timeout) { return tcp_.SendSlice(d, n, timeout); }, stop);
    if (sent.result != net::SendResult::kOk) {
        ESP_LOGW(TAG, "%s handshake request not sent (%d)", SideName(p_.side), static_cast<int>(sent.result));
        return false;
    }
    std::string buffer;
    char chunk[512];
    for (;;) {
        const wsframe::HandshakeStatus s = wsframe::ParseHandshake(&buffer);
        if (s == wsframe::HandshakeStatus::kOk) break;
        if (s == wsframe::HandshakeStatus::kFailed) {
            ESP_LOGW(TAG, "%s handshake refused", SideName(p_.side));
            return false;
        }
        if (stop()) return false;
        const int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
        if (left_ms < 1) {
            ESP_LOGW(TAG, "%s handshake timed out", SideName(p_.side));
            return false;
        }
        bool closed = false;
        const int n = tcp_.ReceiveSlice(chunk, sizeof(chunk), left_ms < 200 ? static_cast<int>(left_ms) : 200, &closed);
        if (n < 0) {
            ESP_LOGW(TAG, "%s handshake: the link ended", SideName(p_.side));
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(n));
    }
    leftover_ = std::move(buffer);
    return true;
}

Link::StartResult Link::Start() {
    const LinkTasks tasks = TasksFor(p_.side);
    last_rx_us_.store(esp_timer_get_time());
    tcp_.OnStream([this](const std::string& data) { rx_.OnBytes(data.data(), data.size()); });
    tcp_.OnDisconnected([this] { rx_.OnDisconnected(); });
    NoticeQueue* notices = p_.notices;
    const Input rx_exit = Exited(p_.attempt, RxBit(p_.side));
    // The bytes that came with the handshake go to the receive task first (its stack, not the
    // worker's: Claude review 149 Minor 3).
    if (!tcp_.StartReceive(tasks.rx, [notices, rx_exit] { notices->Post(rx_exit); }, std::move(leftover_))) {
        return StartResult::kFailed;  // no task runs: the worker may destroy the link
    }
    if (xTaskCreate(TxMain, tasks.tx_name, tasks.tx_stack, this, tasks.tx_priority, nullptr) != pdPASS) {
        // The receive task runs and the link must not be destroyed under it; not expected (memory).
        // Stop it and let the worker restart the device (Claude review 149 Minor 1).
        ESP_LOGE(TAG, "%s: the send task did not start", SideName(p_.side));
        RequestStop();
        return StartResult::kStuck;
    }
    return StartResult::kStarted;
}

void Link::RequestStop() {
    stop_.store(true);
    tcp_.RequestStop();
    p_.queue->Wake();
}

void Link::TxMain(void* arg) {
    Link* link = static_cast<Link*>(arg);
    while (link->tx_.RunOnce()) {
    }
    // The last touch of the link: after the post the manager may destroy it.
    NoticeQueue* notices = link->p_.notices;
    const Input exited = Exited(link->p_.attempt, TxBit(link->p_.side));
    notices->Post(exited);
    vTaskDelete(nullptr);
}

}  // namespace stackchan::link
