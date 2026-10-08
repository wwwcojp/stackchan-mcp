#ifndef _ESP_TCP_H_
#define _ESP_TCP_H_

#include "tcp.h"

#include <atomic>
#include <cstdint>
#include <functional>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#define ESP_TCP_EVENT_RECEIVE_TASK_EXIT 1

// StackChan FW-A2 (design §4.2 change 5): the receive task of one link
struct EspTcpTaskOptions {
    const char* name = "tcp_receive";
    UBaseType_t priority = 1;
    uint32_t stack = 4096;
};

class EspTcp : public Tcp {
public:
    EspTcp();
    ~EspTcp();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

    int GetLastError() override;

    // ---- StackChan FW-A2 (design §4.2 changes 1-5, STACKCHAN_CHANGES.md): a link with
    // deadlines and an explicit lifetime. Connect()/Disconnect()/Send() above are unchanged
    // (HttpClient and the FW-A protocol use them); an object uses either those or these.

    // Change 1: connect to a dotted IPv4 address within timeout_ms, checking stop() every 200 ms.
    // No receive task yet: the caller reads the handshake with ReceiveSlice first.
    bool ConnectManaged(const std::string& ipv4, int port, int timeout_ms, const std::function<bool()>& stop);
    // One receive within timeout_ms before StartReceive (sockslice::RecvSlice).
    int ReceiveSlice(char* buf, size_t len, int timeout_ms, bool* closed);
    // Changes 2, 5: start the receive task with its own name, priority and stack. It receives in
    // 200 ms slices and checks the stop request between them; data goes to the OnStream callback.
    // A passive end (closed by the peer, an error) shuts the socket down and calls the
    // OnDisconnected callback; the socket is closed only when this object is destroyed
    // (change 4). on_exit runs last on the task, after every callback has returned and the
    // stopped bit is set: it must not touch this object. preload (bytes already received with
    // the handshake) goes to the OnStream callback first, on the receive task.
    bool StartReceive(const EspTcpTaskOptions& options, std::function<void()> on_exit, std::string preload = "");
    // Change 3: one send within timeout_us (sockslice::SendSlice). One sending task only.
    int SendSlice(const uint8_t* data, size_t len, int64_t timeout_us);
    // Change 4: ask the receive task to leave (within one slice) / wait until it has.
    void RequestStop();
    bool stop_requested() const { return stop_.load(); }
    bool WaitStopped(int timeout_ms);  // true when the receive task ended (or never started)
    bool link_up() const { return link_up_.load(); }

private:
    int tcp_fd_ = -1;
    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t receive_task_handle_ = nullptr;
    int last_error_ = 0;

    void ReceiveTask();
    // 内部断开处理函数
    // wait_for_task: 是否等待接收任务退出（主动断开为true，被动断开为false）
    void DoDisconnect(bool wait_for_task);

    // StackChan FW-A2
    bool managed_ = false;
    bool receive_started_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<bool> link_up_{false};
    std::function<void()> on_exit_;
    std::string preload_;
    void ManagedReceiveTask();
};

#endif // _ESP_TCP_H_