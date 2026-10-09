#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "connect_inputs_esp.h"
#include "message_stamp.h"
#include "server_time.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <esp_app_desc.h>
#include <esp_system.h>

#define TAG "Application"



Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    // StackChan FW-A2: the queues, the gate, UiController and Outbound (no network yet), before the
    // board is made: its timers start with it and post touches and events (Claude review 163 Minor 7)
    CreateLinkSide();

    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    // StackChan FW-A2 (design §2.4): under audio_queue_mutex_, so only post (audio_queue -> ui_queue)
    callbacks.on_playback_drained = [this]() {
        PostUi(stackchan::ui::EvKind::kPlaybackDrained);
    };
    audio_service_.SetCallbacks(callbacks);
    // StackChan FW-A2: start the audio tasks after the callbacks are set (they read them unlocked;
    // Claude review 161, an observation)
    audio_service_.Start();

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED |
        MAIN_EVENT_UI;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            // StackChan FW-A2 (design §2.4): back to the conversation only when the device really
            // went to Idle
            if (SetDeviceState(kDeviceStateIdle)) {
                PostUi(stackchan::ui::EvKind::kResync);
            }
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        // StackChan FW-A2 (design §2.4): the public entries, routed by the device state now
        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleEntry(stackchan::ui::Entry::kToggleChat);
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleEntry(stackchan::ui::Entry::kStartListening);
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleEntry(stackchan::ui::Entry::kStopListening);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            // StackChan FW-A2 (design §4.1): the mic audio into the audio send queue (never waits;
            // when it is full the oldest mic frames go first)
            stackchan::link::LinkHub* hub = hub_view_.load();
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (hub != nullptr) {
                    hub->PushMic(packet->timestamp, packet->payload.data(), packet->payload.size());
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleEntry(stackchan::ui::Entry::kWakeWord);
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_UI) {
            // One event at a time, until the list is empty (design §2.4)
            while (ui_->ProcessOne()) {
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // StackChan FW-A2 (design §3.5): nothing for the link manager. The pair ends by F1 or a TCP
    // failure, and its LinkDown closes the logical channel.
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    // StackChan FW-A2: the settings from what the assets made (the wake word detector is made when
    // they are applied, on the activation task before this event; Codex review 162)
    ui_->Configure(stackchan::ui::MakeUiConfig(audio_service_));
    // StackChan FW-A2 (design §2.4): the conversation starts here (Resync), only when the device
    // really went to Idle (Activating's inputs were dropped meanwhile)
    if (SetDeviceState(kDeviceStateIdle)) {
        PostUi(stackchan::ui::EvKind::kResync);
    }

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

#if CONFIG_STACKCHAN_SKIP_OTA_CHECK
    if (CONFIG_STACKCHAN_TEST_SEED_ASSETS_URL[0] != '\0') {
        // Test-only build (Kconfig STACKCHAN_TEST_SEED_ASSETS_URL): leave a URL in NVS.
        settings.SetString("download_url", CONFIG_STACKCHAN_TEST_SEED_ASSETS_URL);
        download_url = settings.GetString("download_url");
        ESP_LOGW(TAG, "test: seeded assets download_url");
    }
    // StackChan FW-A (design §2.4): no network at boot. A URL left in NVS (e.g. by an
    // earlier OTA reply) is kept but not downloaded; local assets are still applied.
    if (!download_url.empty()) {
        ESP_LOGI(TAG, "assets download skipped (CONFIG_STACKCHAN_SKIP_OTA_CHECK): %s",
                 download_url.c_str());
        download_url.clear();
    }
#endif

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
#if CONFIG_STACKCHAN_SKIP_OTA_CHECK
    // StackChan FW-A (design §2.4): no OTA/activation query at boot. Keep the
    // local steps the query used to perform as side effects.
    ota_->LoadCurrentVersion();
    ota_->MarkCurrentVersionValid();
    ESP_LOGI(TAG, "OTA check skipped (CONFIG_STACKCHAN_SKIP_OTA_CHECK)");
    return;
#endif
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    // StackChan FW-A2 (design §3.5): the link objects are made once per boot. A second call (back
    // from the Wi-Fi settings) restarts instead: the running tasks could not be destroyed safely.
    if (protocol_initialized_) {
        ESP_LOGW(TAG, "InitializeProtocol again: restarting");
        esp_restart();
    }
    protocol_initialized_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    uint32_t boot_count = 0;
    {
        // StackChan FW-A (design §2.2): persistent boot counter for fw_epoch. Settings commits in
        // its destructor, so it is closed before anything that may restart.
        Settings settings("stackchan", true);
        boot_count = stackchan::NextBootCount(settings.GetInt("boot_count", 0));
        settings.SetInt("boot_count", stackchan::BootCountToStore(boot_count));
    }
    ESP_LOGI(TAG, "StackChan boot_count=%u", static_cast<unsigned>(boot_count));

    // The receive side's app ports (plan 2B-2a handoff 5): what the application does with the
    // messages of the pair. The receive tasks call them; main-task work goes through Schedule.
    stackchan::link::AppDeps app;
    app.gate = gate_.get();
    app.ctrl_queue = ctrl_queue_.get();
    app.notices = notices_.get();
    app.now_us = []() { return esp_timer_get_time(); };
    app.apply_server_time = [](const cJSON* server_time) {
        // StackChan FW-A (design §2.6): optional wall-clock time from the gateway. Never fails
        // the hello: a missing or invalid server_time leaves the clock and TZ untouched.
        stackchan::ServerTime time;
        if (!stackchan::ParseServerTime(server_time, &time)) {
            ESP_LOGW(TAG, "server_time in hello is invalid; ignored");
            return;
        }
        auto result = stackchan::ApplyServerTime(time);
        if (result == stackchan::ApplyResult::kOk) {
            // Read the clock back: the acceptance compares it with the UTC that was sent.
            // The newlib nano printf cannot format a 64-bit %lld (it prints "ld"), so the
            // 64-bit value goes through std::to_string.
            ESP_LOGI(TAG, "server_time set (hello): clock_utc_ms=%s offset_min=%d",
                     std::to_string(stackchan::ReadClockUtcMs()).c_str(),
                     static_cast<int>(time.offset_min));
        } else {
            ESP_LOGW(TAG, "server_time (hello): %s", stackchan::ApplyResultText(result));
        }
    };
    app.on_audio_only = [this]() {
        // kAudioOnly brings no LinkUp: wake the power save timer anyway (design §2.4)
        Schedule([]() {
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        });
    };
    app.push_server_audio = [this](std::unique_ptr<AudioStreamPacket>& packet) {
        return audio_service_.PushServerAudio(packet);
    };
    app.post_app = [this](stackchan::link::AppMessage message) {
        Schedule([this, message = std::move(message)]() {
            ShowAppMessage(message);
        });
    };
    app.avatar_set_fetch = [](const cJSON* root) {
        Board::GetInstance().OnAvatarSetFetch(root);
    };
    app.mcp = [](uint64_t e, const cJSON* payload) {
        McpServer::GetInstance().ParseMessage(e, payload);
    };
    app.stat_inputs = [this]() {
        return CollectStat();
    };
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
    app.receive_custom = true;
#endif
    app_link_ = std::make_unique<stackchan::link::AppLink>(app);
    if (const char* missing = app_link_->Missing()) {
        // An empty port would abort on the first message (Claude review 156 Minor 5)
        ESP_LOGE(TAG, "app port %s is empty: the link is not started", missing);
        return;
    }

    stackchan::link::HubDeps hub;
    hub.notices = notices_.get();
    hub.audio_queue = audio_queue_.get();
    hub.ctrl_queue = ctrl_queue_.get();
    hub.gate = gate_.get();
    hub.ui = ui_.get();
    hub.boot_count = boot_count;
    hub.app = app_link_->Ports();
    hub.read_inputs = []() {
        return stackchan::link::ReadConnectInputs();
    };
    hub.server_aec = aec_mode_ == kAecOnServerSide;
    hub.frame_duration_ms = OPUS_FRAME_DURATION_MS;
    hub_ = std::make_unique<stackchan::link::LinkHub>(hub);
    if (!hub_->Start()) {
        ESP_LOGE(TAG, "the link manager task did not start");
        return;
    }
    hub_view_.store(hub_.get());
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening(ListeningProfile profile) {
    if (profile != kListeningProfileVoice) {
        ESP_LOGW(TAG, "StartListening: a device-started listening is voice (profile %d not used)",
                 static_cast<int>(profile));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::Touch() {
    // The board classified it (ui::ClassifyTouch) on its timer task; UiController drops it when
    // the conversation is suspended meanwhile
    if (ui_) {
        PostUi(stackchan::ui::EvKind::kTouch);
    }
}

void Application::ToggleAudioTesting() {
    // Design §2.4: decided on the main task with the state then (a touch that waited may find the
    // screen changed: logged and dropped, never a conversation Toggle)
    Schedule([this]() {
        auto target = stackchan::ui::AudioTestingToggleTarget(GetDeviceState());
        if (!target.has_value()) {
            ESP_LOGI(TAG, "audio test toggle dropped in state %d", static_cast<int>(GetDeviceState()));
            return;
        }
        audio_service_.EnableAudioTesting(*target == kDeviceStateAudioTesting);
        SetDeviceState(*target);
    });
}

void Application::EnterNonConversation(DeviceState target) {
    if (ui_) {
        ui_->EnterNonConversation(target);
    } else {
        SetDeviceState(target);
    }
}

void Application::PostUi(stackchan::ui::EvKind kind) {
    stackchan::ui::Event ev;
    ev.kind = kind;
    ui_->Post(ev);
}

void Application::HandleEntry(stackchan::ui::Entry entry) {
    namespace ui = stackchan::ui;
    const DeviceState state = GetDeviceState();
    switch (ui::RouteEntry(entry, state)) {
        case ui::EntryRoute::kTouch:
            PostUi(ui::EvKind::kTouch);
            break;
        case ui::EntryRoute::kToggle:
            PostUi(ui::EvKind::kToggle);
            break;
        case ui::EntryRoute::kWakeWord: {
            ESP_LOGI(TAG, "Wake word: %s", audio_service_.GetLastWakeWord().c_str());
            ui::Event ev;
            ev.kind = ui::EvKind::kWakeWord;
            ev.mode = GetDefaultListeningMode() == kListeningModeRealtime ? ui::Mode::kRealtime
                                                                          : ui::Mode::kAutoStop;
            ui_->Post(ev);
            break;
        }
        case ui::EntryRoute::kToggleAudioTesting:
            ToggleAudioTesting();
            break;
        case ui::EntryRoute::kDrop:
            ESP_LOGI(TAG, "entry %d dropped in state %d", static_cast<int>(entry), static_cast<int>(state));
            break;
    }
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    // StackChan FW-A2 (design §2.4): in the conversation states only the look stays here (the last
    // state's look wins when changes come together). The mic, the listen messages, the queues,
    // the sounds and the wake word detector are UiController's.
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableRawCapture(false);
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // StackChan FW-A2 (design §3.5): nothing is destroyed (esp_restart calls no destructor); the
    // pair is ended first so the gateway sees it go
    EndLinks();
    audio_service_.Stop();
    esp_restart();
}

void Application::EndLinks() {
    stackchan::link::LinkHub* hub = hub_view_.load();
    if (hub == nullptr) {
        return;
    }
    hub->RequestShutdown();
    // Until the pair is over and none of its tasks runs (the links sent their close), at most 1 s
    // (today's vTaskDelay(1000)); restart even when it is not over by then. Not TransportConnected:
    // it drops as soon as the end is decided (Claude review 163 Important 1)
    for (int i = 0; i < 20 && !hub->Stopped(); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // StackChan FW-A2 (design §3.5): leave the conversation (the main task: MCP's
    // self.upgrade_firmware runs there; CheckNewVersion's call is skipped by
    // CONFIG_STACKCHAN_SKIP_OTA_CHECK), end the pair, then fetch with HttpClient
    EnterNonConversation(kDeviceStateUpgrading);
    EndLinks();
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // StackChan FW-A2 (design §3.5): the pair is over and the conversation suspended, so restart
        // (today stayed in Upgrading with no way back)
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting...");
        audio_service_.Start();
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    // StackChan FW-A2 (design §2.4 v10): the WakeWord event (nothing is encoded or sent)
    ESP_LOGI(TAG, "WakeWordInvoke: %s", wake_word.c_str());
    xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    // Block sleep while the gateway link is alive (MCP stays usable). StackChan FW-A2 (design
    // §1.2): the link manager's view, a bound pair or the audio link alone (kAudioOnly).
    stackchan::link::LinkHub* hub = hub_view_.load();
    if (hub != nullptr && hub->TransportConnected()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(uint64_t e, const std::string& payload) {
    if (!outbound_) {
        return;
    }
    auto r = outbound_->McpReply(e, payload);
    if (r != stackchan::link::OutResult::kQueued) {
        ESP_LOGW(TAG, "mcp reply not sent (%d)", static_cast<int>(r));
    }
}

void Application::SendStackChanEvent(
    const char* event_type, const char* subtype, uint64_t duration_ms) {
    // The head-touch poll calls this on the shared esp_timer task: build and queue on the main
    // task, as FW-A did, so the timer task neither runs cJSON nor waits on the gate and queues
    std::string event_type_str = event_type ? event_type : "";
    std::string subtype_str = subtype ? subtype : "";
    const int64_t ts_ms = esp_timer_get_time() / 1000;
    Schedule([this, event_type_str, subtype_str, duration_ms, ts_ms]() {
        if (!outbound_) {
            return;
        }
        outbound_->StackChanEvent(event_type_str, subtype_str, duration_ms, ts_ms);
    });
}

void Application::SendJsonString(const std::string& json_str) {
    // A board notice such as avatar_set_loaded: the pair bound now; not a JSON object: dropped
    if (!outbound_) {
        return;
    }
    if (outbound_->JsonString(json_str) == stackchan::link::OutResult::kBadJson) {
        ESP_LOGW(TAG, "SendJsonString: not a JSON object, dropped");
    }
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // StackChan FW-A2: the hello's aec feature is fixed when the link hub is made
        // (HubDeps::server_aec); the logical channel is UiController's and stays as it is
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    // StackChan FW-A2 (design §3.5): the link objects stay (a second InitializeProtocol restarts);
    // the pair ends and no reconnect follows
    stackchan::link::LinkHub* hub = hub_view_.load();
    if (hub != nullptr) {
        hub->RequestShutdown();
    }
}

// StackChan FW-A2 (design §1, §2): the queues, the gate, UiController and Outbound. No network;
// the link hub that uses them comes in InitializeProtocol.
void Application::CreateLinkSide() {
    namespace gate = stackchan::gate;
    namespace link = stackchan::link;
    namespace net = stackchan::net;
    namespace ui = stackchan::ui;

    notices_ = std::make_unique<link::NoticeQueue>();
    audio_queue_ = std::make_unique<net::SendQueue>(net::kAudioLimits);
    ctrl_queue_ = std::make_unique<net::SendQueue>(net::kCtrlLimits);

    gate::GatePorts ports;
    ports.audio = audio_service_.playback_sink();
    ports.audio_queue = audio_queue_.get();
    ports.ctrl_queue = ctrl_queue_.get();
    ports.now_us = []() { return esp_timer_get_time(); };
    // Under the gate lock: only post (gate -> ui_queue, design §1.2)
    ports.post_gate_changed = [this](uint64_t e, bool spk, uint32_t rev) {
        ui::Event ev;
        ev.kind = ui::EvKind::kGateChanged;
        ev.e = e;
        ev.spk = spk;
        ev.rev = rev;
        ui_->Post(ev);
    };
    ports.post_link_up = [this](uint64_t e, bool spk, uint32_t rev) {
        ui::Event ev;
        ev.kind = ui::EvKind::kLinkUp;
        ev.e = e;
        ev.spk = spk;
        ev.rev = rev;
        ui_->Post(ev);
    };
    ports.end_pair = [this](uint64_t e, link::EndReason reason, bool flush) {
        link::Input in;
        in.kind = link::InKind::kEndRequest;
        in.e = e;
        in.reason = reason;
        in.flush = flush;
        notices_->Post(in);  // never waits; a full list restarts (design §3.8)
    };
    gate_ = std::make_unique<gate::PlaybackGate>(ports);

    ui::UiEspDeps deps;
    deps.app = this;
    deps.audio = &audio_service_;
    deps.gate = gate_.get();
    deps.audio_queue = audio_queue_.get();
    deps.notices = notices_.get();
    deps.listening_led = [](bool on) {
        Board::GetInstance().SetListeningLed(on);
    };
    ui_ports_ = std::make_unique<ui::UiPortsEsp>(deps);
    // The settings depend on the assets (the wake word detector): Configure at the activation done
    ui_ = std::make_unique<ui::UiController>(ui_ports_.get(), ui::UiConfig{});
    ui_ports_->BindUi(ui_.get());
    ui_->SetWake([this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_UI);
    });

    link::OutboundDeps out;
    out.gate = gate_.get();
    out.audio_queue = audio_queue_.get();
    out.notices = notices_.get();
    out.now_us = []() { return esp_timer_get_time(); };
    out.bound_pair = [this]() {
        link::LinkHub* hub = hub_view_.load();
        return hub != nullptr ? hub->BoundPair() : uint64_t{0};
    };
    outbound_ = std::make_unique<link::Outbound>(out);
}

// What the receive side's app JSON asks of the main task (today's OnIncomingJson)
void Application::ShowAppMessage(const stackchan::link::AppMessage& message) {
    using Kind = stackchan::link::AppMessage::Kind;
    auto display = Board::GetInstance().GetDisplay();
    switch (message.kind) {
        case Kind::kAssistantText:
            ESP_LOGI(TAG, "<< %s", message.text.c_str());
            display->SetChatMessage("assistant", message.text.c_str());
            break;
        case Kind::kUserText:
            ESP_LOGI(TAG, ">> %s", message.text.c_str());
            display->SetChatMessage("user", message.text.c_str());
            break;
        case Kind::kEmotion:
            display->SetEmotion(message.emotion.c_str());
            break;
        case Kind::kAlert:
            // The sound never waits here: on the main task a full decode queue (a playback) would
            // hold up the UI events (Claude review 163 Minor 6)
            Alert(message.status.c_str(), message.text.c_str(), message.emotion.c_str());
            if (!audio_service_.PlaySoundNoWait(Lang::Sounds::OGG_VIBRATION)) {
                ESP_LOGW(TAG, "alert sound: the decode queue was full");
            }
            break;
        case Kind::kReboot:
            Reboot();
            break;
        case Kind::kCustom:
            display->SetChatMessage("system", message.text.c_str());
            break;
    }
}

// stat (contract §5.1, design §6.2; plan 2B-2a handoff 3): each source under its own lock only,
// one after another (no lock held while another is taken). The control receive task calls it.
stackchan::link::StatInputs Application::CollectStat() {
    stackchan::link::StatInputs in;
    in.gate = gate_->Snapshot();
    in.gate_stats = gate_->Stats();
    in.book = audio_service_.PlaybackBookSnapshot();
    in.pipeline = audio_service_.PlaybackStats();
    in.ui = ui_->stats();
    stackchan::link::LinkHub* hub = hub_view_.load();
    if (hub != nullptr) {
        hub->FillStat(&in);
    }
    in.app = app_link_->stats();
    const stackchan::link::OutboundStats out = outbound_->stats();
    in.app.out_unbound = out.unbound;
    in.app.out_closed = out.closed;
    in.app.out_bad_json = out.bad_json;
    in.heap_free = esp_get_free_heap_size();
    in.heap_min = esp_get_minimum_free_heap_size();
    char sha[9] = {};
    esp_app_get_elf_sha256(sha, sizeof(sha));
    in.build = std::string(esp_app_get_description()->version) + " " + sha;
    return in;
}
