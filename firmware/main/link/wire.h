// StackChan FW-A2: the JSON messages of the K151-DeviceLink contract (contract §1 S1-S8, §2.1,
// §5.1) and the device-side listen messages, as pure parse/build functions over cJSON. No
// ESP-IDF, no locks: the receive tasks parse, the send tasks stamp fw_epoch/seq in send order
// (design §4.1), everybody else builds and queues.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct cJSON;

namespace stackchan::wire {

// ---- receive side ----

// The "type" of a message ("" when missing or not a string).
std::string TypeOf(const cJSON* root);

// session_id matches (a non-empty current session id and the same string). Messages from the
// server that carry a session_id are dropped when this is false (today's IsAbortRequest check).
bool SessionMatches(const cJSON* root, const std::string& session_id);

// The audio hello reply offers the control mode: "stackchan_ctrl" is the number 1 (contract S2).
bool CtrlOffered(const cJSON* hello_reply);

// {"type":"hello","role":"control","audio_epoch":E} (contract S4.1). False unless the type, the
// role and a non-negative integer audio_epoch are there.
bool ParseCtrlHelloReply(const cJSON* root, uint64_t* audio_epoch);

struct TtsStart {
    uint32_t gen = 0;
    uint32_t aborted_gen = 0;
    uint32_t dev_abort_seen = 0;
};
// {"type":"tts","state":"start","gen":N,"aborted_gen":A,"dev_abort_seen":K} (contract §2.1).
// False when a field is missing, not an integer, out of range, or gen is 0. A missing
// aborted_gen / dev_abort_seen is not allowed (the contract always sends them).
bool ParseTtsStart(const cJSON* root, TtsStart* out);
// {"type":"tts","state":"stop","gen":N}
bool ParseTtsStop(const cJSON* root, uint32_t* gen);

constexpr size_t kMaxReqIdLen = 32;  // contract §2.1

struct AbortRequest {
    uint32_t gen = 0;
    std::string req_id;
    std::string reason;      // "hush" | "user" (other strings are kept as they are)
    std::string session_id;  // "" when missing; the gate drops a request of another session
};
// {"type":"abort","session_id":S,"gen":g,"req_id":ID,"reason":R} without "state" (a reply is
// never a request). False when gen is missing / 0, or req_id is missing, empty or longer than
// kMaxReqIdLen. The session is not checked here: the gate compares it with the pair's
// (contract R1, S8; Codex review 143 Important 2).
bool ParseAbortRequest(const cJSON* root, AbortRequest* out);

// {"type":"stat","req_id":ID} without "state"
bool ParseStatRequest(const cJSON* root, std::string* req_id);

enum class ListenMode { kManualStop, kAutoStop, kRealtime };
enum class ListenProfile { kVoice, kRaw };
struct GwListen {
    bool start = false;  // false: stop
    ListenMode mode = ListenMode::kManualStop;
    ListenProfile profile = ListenProfile::kVoice;
    bool mode_unknown = false;     // a "mode" that is not a known string (ManualStop is used)
    bool profile_unknown = false;  // a "profile" that is not a known string (voice is used)
};
// {"type":"listen","state":"start"|"stop","mode":"manual"|"auto"|"realtime","profile":"voice"|"raw"}
// from the gateway. A missing mode is ManualStop (the gateway controls the stop boundary, as
// today), a missing profile is voice (today's ParseListenProfile).
bool ParseGwListen(const cJSON* root, GwListen* out);

// ---- build side (the caller owns the result; fw_epoch/seq are stamped by the send task) ----

// Adds "stackchan_ctrl": 1 to the audio hello's "features" object (contract S1), creating the
// object when missing. No-op unless root is an object.
void AddCtrlFeature(cJSON* audio_hello);

// {"type":"hello","role":"control","audio_epoch":E} (contract S3)
cJSON* BuildCtrlHello(uint64_t e);
// {"type":"ready","audio_epoch":E} (contract S4.2)
cJSON* BuildReady(uint64_t e);

// {"type":"abort","state":"done","req_id":ID,"reason":R,"gen":g,"result":"stopped"|"already",
//  "dropped_ms":D} (contract §2.1). dropped_ms is 0 for "already".
cJSON* BuildAbortDone(const AbortRequest& req, bool already, uint32_t dropped_ms);

enum class DeviceAbortReason { kTouch, kWakeWord };
// {"session_id":S,"type":"abort","gen":g,"dev_abort_seq":k} (+ "reason":"wake_word_detected"
// for a wake word, as today's SendAbortSpeaking). Contract §2.1, R5.
cJSON* BuildDeviceAbort(const std::string& session_id, uint32_t gen, uint32_t dev_abort_seq,
                        DeviceAbortReason reason);

// {"session_id":S,"type":"listen","state":"start","mode":"manual"|"auto"|"realtime"}
cJSON* BuildListenStart(const std::string& session_id, ListenMode mode);
// {"session_id":S,"type":"listen","state":"stop"}
cJSON* BuildListenStop(const std::string& session_id);

// The stat reply (contract §5.1, design §6.2): {"type":"stat","state":"done","req_id":ID,
// "<group>":{"<name>":value,...},...}. The groups and names are the shell's (plan 2B fills them);
// this only fixes the shape. Values are non-negative integers.
struct StatItem {
    const char* name;
    uint64_t value;
};
struct StatGroup {
    const char* name;
    std::vector<StatItem> items;
};
cJSON* BuildStatReply(const std::string& req_id, const std::vector<StatGroup>& groups);

// ---- fw_epoch / seq (contract S8, FW-A §2.2) ----

// The control link keeps the fw_epoch E of the audio connection it was opened for and counts its
// own seq from 1 (hello 1, ready 2). It never moves conn_index.
class CtrlStamp {
public:
    explicit CtrlStamp(uint64_t e) : e_(e) {}
    // Writes/overwrites "fw_epoch" and "seq" (+1 per call). No-op unless root is an object.
    void Stamp(cJSON* root);
    uint32_t seq() const { return seq_; }

private:
    uint64_t e_;
    uint32_t seq_ = 0;
};

}  // namespace stackchan::wire
