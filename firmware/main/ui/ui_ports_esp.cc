// StackChan FW-A2 plan 2B-2a: UiController's ports on the ESP (see ui_ports_esp.h).
#include "ui_ports_esp.h"

#include <cJSON.h>
#include <esp_log.h>

#include <string>
#include <utility>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "tx_loop.h"

#define TAG "UiPortsEsp"

namespace stackchan::ui {

UiConfig MakeUiConfig(AudioService& audio) {
    UiConfig c;
#if CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    c.wake_in_listening = audio.IsAfeWakeWord();
#endif
    c.wake_in_speaking = audio.IsAfeWakeWord();
#if CONFIG_SEND_WAKE_WORD_DATA
    c.send_wake_word_data = true;  // FW-A2 builds switch it off (design §2.4 v10, plan 2B-2b)
#endif
    return c;
}

UiPortsEsp::UiPortsEsp(UiEspDeps deps) : d_(std::move(deps)) {
    esp_timer_create_args_t args = {};
    args.callback = &UiPortsEsp::OnTimer;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "listen_timeout";
    if (esp_timer_create(&args, &timer_) != ESP_OK) {
        ESP_LOGE(TAG, "listen timer not created");
        timer_ = nullptr;
    }
}

UiPortsEsp::~UiPortsEsp() {
    if (timer_ != nullptr) {
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
    }
}

// The timer task: post only (design §1.2)
void UiPortsEsp::OnTimer(void* arg) {
    auto* self = static_cast<UiPortsEsp*>(arg);
    Event ev;
    ev.kind = EvKind::kListenTimeout;
    ev.req = self->timer_req_.load();
    if (esp_timer_get_time() < self->timer_due_us_.load() - kTimerSlackUs) return;  // an earlier arm's
    self->d_.ui->Post(ev);
}

DeviceState UiPortsEsp::CurrentState() { return d_.app->GetDeviceState(); }

bool UiPortsEsp::SetDeviceState(DeviceState s) { return d_.app->SetDeviceState(s); }

gate::TouchResult UiPortsEsp::GateTouch(uint64_t e, wire::ListenMode mode, wire::DeviceAbortReason reason) {
    return d_.gate->OnTouch(e, mode, reason);
}

void UiPortsEsp::ClearForListening() { d_.gate->ClearForListening(); }

// A JSON of the pair into the audio send queue; a full queue ends the pair (design §4.1)
void UiPortsEsp::Queue(uint64_t e, cJSON* root, const char* what) {
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) {
        ESP_LOGE(TAG, "%s not printed", what);
        return;
    }
    std::string json(text);
    cJSON_free(text);
    gate::PlaybackGate* g = d_.gate;
    link::NoticeQueue* n = d_.notices;
    const net::PushResult r = link::QueueJsonOrEnd(
        *d_.audio_queue, e, std::move(json), esp_timer_get_time(),
        [g](uint64_t pair, link::EndReason reason) { g->StopForDeath(pair, reason); },
        [n](const link::Input& in) { return n->Post(in); });
    if (r != net::PushResult::kQueued) ESP_LOGW(TAG, "%s not queued (%d)", what, static_cast<int>(r));
}

void UiPortsEsp::SendListenStart(uint64_t e, Mode mode) {
    Queue(e, wire::BuildListenStart(d_.gate->session_id(), ToWireMode(mode)), "listen start");
}

void UiPortsEsp::SendListenStop(uint64_t e) { Queue(e, wire::BuildListenStop(d_.gate->session_id()), "listen stop"); }

void UiPortsEsp::SendWakeWord(uint64_t /*e*/) {
    // FW-A2 never sends the wake word audio (design §2.4 v10): only reached with
    // CONFIG_SEND_WAKE_WORD_DATA, which the FW-A2 build switches off
    ESP_LOGW(TAG, "wake word audio is not sent in FW-A2");
}

// The listening source (today's Listening transition, application.cc:1125-1144)
void UiPortsEsp::MicOn(Profile p) {
    if (p == Profile::kRaw) {
        d_.audio->EnableVoiceProcessing(false);
        d_.audio->EnableRawCapture(true);
    } else {
        d_.audio->EnableRawCapture(false);
        d_.audio->EnableVoiceProcessing(true);
    }
}

void UiPortsEsp::MicOff() {
    d_.audio->EnableRawCapture(false);
    d_.audio->EnableVoiceProcessing(false);
}

void UiPortsEsp::PlayPopup() {
    if (!d_.audio->PlaySoundNoWait(Lang::Sounds::OGG_POPUP)) ESP_LOGW(TAG, "popup: the decode queue was full");
}

void UiPortsEsp::RequestDrain() { d_.audio->RequestPlaybackDrain(); }

void UiPortsEsp::WakeDetect(bool on) { d_.audio->EnableWakeWordDetection(on); }

void UiPortsEsp::ArmTimer(uint32_t req) {
    if (timer_ == nullptr) return;
    esp_timer_stop(timer_);
    timer_due_us_.store(esp_timer_get_time() + kListenTimeoutUs);
    timer_req_.store(req);
    esp_timer_start_once(timer_, kListenTimeoutUs);
}

void UiPortsEsp::CancelTimer() {
    if (timer_ != nullptr) esp_timer_stop(timer_);
}

// "接続待ち" (design §2.4): no pair yet; today's Connecting status text
void UiPortsEsp::ShowWaitingForLink() {
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::CONNECTING);
}

void UiPortsEsp::StopMouth() { Board::GetInstance().OnTtsStop(); }

void UiPortsEsp::StartMouth() { Board::GetInstance().OnTtsStart(); }

void UiPortsEsp::SetListeningLed(bool on) {
    if (d_.listening_led) d_.listening_led(on);
}

void UiPortsEsp::SetPerformance(bool performance) {
    Board::GetInstance().SetPowerSaveLevel(performance ? PowerSaveLevel::PERFORMANCE : PowerSaveLevel::LOW_POWER);
}

}  // namespace stackchan::ui
