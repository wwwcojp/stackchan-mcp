#include "esp_tcp.h"

#include <esp_log.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

#include "sock_slice.h"

static const char *TAG = "EspTcp";

EspTcp::EspTcp() {
    event_group_ = xEventGroupCreate();
}

EspTcp::~EspTcp() {
    if (managed_) {
        // StackChan FW-A2 (change 4): the owner destroys a managed link only after its receive
        // task has ended, so the socket is closed here and nowhere else.
        if (tcp_fd_ != -1) {
            close(tcp_fd_);
            tcp_fd_ = -1;
        }
    } else {
        Disconnect();
    }

    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool EspTcp::Connect(const std::string& host, int port) {
    // 确保先断开已有连接
    if (connected_) {
        Disconnect();
    }

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent *server = gethostbyname(host.c_str());
    if (server == NULL) {
        last_error_ = h_errno;
        ESP_LOGE(TAG, "Failed to get host by name");
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);
    ESP_LOGI(TAG, "Resolved %s -> %s", host.c_str(), inet_ntoa(*(struct in_addr *)server->h_addr));

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    int ret = connect(tcp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }

    connected_ = true;

    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    xTaskCreate([](void* arg) {
        EspTcp* tcp = (EspTcp*)arg;
        tcp->ReceiveTask();
        xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
        vTaskDelete(NULL);
    }, "tcp_receive", 4096, this, 1, &receive_task_handle_);
    return true;
}

void EspTcp::Disconnect() {
    // 如果已经断开，直接返回
    if (!connected_) {
        return;
    }

    // 主动断开，需要等待接收任务退出
    DoDisconnect(true);
}

void EspTcp::DoDisconnect(bool wait_for_task) {
    connected_ = false;

    if (tcp_fd_ != -1) {
        close(tcp_fd_);
        tcp_fd_ = -1;

        // 只有主动断开时才需要等待接收任务退出
        // 被动断开时，当前就是接收任务，不需要等待
        if (wait_for_task) {
            auto bits = xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
            if (!(bits & ESP_TCP_EVENT_RECEIVE_TASK_EXIT)) {
                ESP_LOGE(TAG, "Failed to wait for receive task exit");
            }
        }
    }

    // 断开连接时触发断开回调
    if (disconnect_callback_) {
        disconnect_callback_();
    }
}

int EspTcp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();

    while (total_sent < data_size) {
        int ret = send(tcp_fd_, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret <= 0) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = recv(tcp_fd_, data.data(), data.size(), 0);
        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "TCP receive failed: %d", ret);
            }
            // 被动断开，不需要等待接收任务退出（当前就是接收任务）
            DoDisconnect(false);
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}

int EspTcp::GetLastError() {
    return last_error_;
}

// ---- StackChan FW-A2 (design §4.2 changes 1-5) ----

bool EspTcp::ConnectManaged(const std::string& ipv4, int port, int timeout_ms, const std::function<bool()>& stop) {
    managed_ = true;
    int err = 0;
    tcp_fd_ = sockslice::ConnectWithin(ipv4.c_str(), static_cast<uint16_t>(port), timeout_ms, stop, &err);
    if (tcp_fd_ < 0) {
        last_error_ = err;
        ESP_LOGW(TAG, "Connect to %s:%d failed: errno=%d", ipv4.c_str(), port, err);
        return false;
    }
    link_up_.store(true);
    return true;
}

int EspTcp::ReceiveSlice(char* buf, size_t len, int timeout_ms, bool* closed) {
    return sockslice::RecvSlice(tcp_fd_, buf, len, timeout_ms, closed);
}

bool EspTcp::StartReceive(const EspTcpTaskOptions& options, std::function<void()> on_exit, std::string preload) {
    on_exit_ = std::move(on_exit);
    preload_ = std::move(preload);
    xEventGroupClearBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
    const BaseType_t ok = xTaskCreate([](void* arg) {
        EspTcp* tcp = static_cast<EspTcp*>(arg);
        tcp->ManagedReceiveTask();
        {
            // Take what is needed off the object before the stopped bit: after it the owner may
            // destroy the object. The scope ends before vTaskDelete, which never returns, so the
            // function object is destroyed (no leak per link: Codex review 148 Important 1).
            std::function<void()> on_exit = std::move(tcp->on_exit_);
            xEventGroupSetBits(tcp->event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT);
            if (on_exit) {
                on_exit();
            }
        }
        vTaskDelete(NULL);
    }, options.name, options.stack, this, options.priority, &receive_task_handle_);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to start the receive task %s", options.name);
        on_exit_ = nullptr;
        return false;
    }
    receive_started_ = true;
    return true;
}

void EspTcp::ManagedReceiveTask() {
    if (!preload_.empty() && stream_callback_) {
        stream_callback_(preload_);  // the bytes that came with the handshake, on this task
    }
    preload_.clear();
    std::string data;
    while (!stop_.load()) {
        data.resize(1500);
        bool closed = false;
        const int n = sockslice::RecvSlice(tcp_fd_, data.data(), data.size(), sockslice::kSliceMs, &closed);
        if (n == 0) {
            continue;  // a slice passed: look at the stop request again (change 2)
        }
        if (n < 0) {
            // A passive end: shut the socket down but keep the descriptor (change 4), so a send
            // task still using it never meets a reused descriptor number.
            if (!closed) {
                ESP_LOGW(TAG, "Receive failed: errno=%d", errno);
            }
            link_up_.store(false);
            shutdown(tcp_fd_, SHUT_RDWR);
            if (disconnect_callback_) {
                disconnect_callback_();
            }
            break;
        }
        data.resize(n);
        if (stream_callback_) {
            stream_callback_(data);
        }
    }
}

int EspTcp::SendSlice(const uint8_t* data, size_t len, int64_t timeout_us) {
    return sockslice::SendSlice(tcp_fd_, data, len, timeout_us);
}

void EspTcp::RequestStop() {
    stop_.store(true);
}

bool EspTcp::WaitStopped(int timeout_ms) {
    if (!receive_started_) {
        return true;
    }
    const EventBits_t bits = xEventGroupWaitBits(event_group_, ESP_TCP_EVENT_RECEIVE_TASK_EXIT, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(timeout_ms));
    return (bits & ESP_TCP_EVENT_RECEIVE_TASK_EXIT) != 0;
}
