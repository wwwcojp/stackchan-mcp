// StackChan FW-A2 §7.1-5: replay the TLA+ model's traces (stackchan-works models/fw_a2,
// converted by scripts/tlc_traces.py, copied into model_traces/ with a MANIFEST) through the
// C++ Step, and check that the UiController state matches the model after every event.
// Fails when no trace ran, when the executed trace set differs from MANIFEST, or on any
// unknown event / malformed step.
#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

#include "cJSON.h"
#include "ui_controller_core.h"

using namespace stackchan::ui;

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

using Key = std::tuple<std::string, std::string, int>;  // (file, trace name, steps run)

std::set<Key> ReadManifest() {
    std::set<Key> out;
    std::istringstream in(ReadFile(std::string(MODEL_TRACES_DIR) + "/MANIFEST"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto t1 = line.find('\t');
        const auto t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) {
            ADD_FAILURE() << "malformed MANIFEST line: " << line;
            continue;
        }
        out.insert({line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1), std::stoi(line.substr(t2 + 1))});
    }
    return out;
}

bool Uint(const cJSON* o, const char* k, uint32_t* out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble != static_cast<double>(v->valueint)) return false;
    *out = static_cast<uint32_t>(v->valueint);
    return true;
}
// E is 64-bit (design §3.1); the traces' epochs are small
bool Uint(const cJSON* o, const char* k, uint64_t* out) {
    uint32_t v = 0;
    if (!Uint(o, k, &v)) return false;
    *out = v;
    return true;
}
bool Bool(const cJSON* o, const char* k, bool* out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsBool(v)) return false;
    *out = cJSON_IsTrue(v);
    return true;
}
std::string Str(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

bool ParseMode(const std::string& m, Mode* out) {
    if (m == "manual") *out = Mode::kManualStop;
    else if (m == "auto") *out = Mode::kAutoStop;
    else return false;
    return true;
}

bool ParseEvent(const cJSON* j, Event* ev) {
    const std::string kind = Str(j, "kind");
    const int n = cJSON_GetArraySize(j);
    if (kind == "touch" || kind == "toggle" || kind == "wake_word") {
        ev->kind = kind == "touch" ? EvKind::kTouch : kind == "toggle" ? EvKind::kToggle : EvKind::kWakeWord;
        const std::string reply = Str(j, "reply");
        if (reply == "r5") ev->reply = TouchReply::kR5;
        else if (reply == "send_failed") ev->reply = TouchReply::kSendFailed;
        else if (reply == "unbound") ev->reply = TouchReply::kUnbound;
        else if (reply == "not_speaking") ev->reply = TouchReply::kNotSpeaking;
        else return false;
        if (!Uint(j, "rev", &ev->rev)) return false;
        if (kind == "wake_word") return ParseMode(Str(j, "mode"), &ev->mode) && n == 4;
        return n == 3;
    }
    if (kind == "gate_changed" || kind == "link_up") {
        ev->kind = kind == "link_up" ? EvKind::kLinkUp : EvKind::kGateChanged;
        return Uint(j, "e", &ev->e) && Bool(j, "spk", &ev->spk) && Uint(j, "rev", &ev->rev) && n == 4;
    }
    if (kind == "link_down" || kind == "gw_stop") {
        ev->kind = kind == "link_down" ? EvKind::kLinkDown : EvKind::kGwListenStop;
        return Uint(j, "e", &ev->e) && n == 2;
    }
    if (kind == "gw_start") {
        ev->kind = EvKind::kGwListenStart;
        return Uint(j, "e", &ev->e) && ParseMode(Str(j, "mode"), &ev->mode) && n == 3;
    }
    if (kind == "timeout") {
        ev->kind = EvKind::kListenTimeout;
        return Uint(j, "req", &ev->req) && n == 2;
    }
    if (kind == "drained") {
        ev->kind = EvKind::kPlaybackDrained;
        return n == 1;
    }
    if (kind == "resync") {
        ev->kind = EvKind::kResync;
        return n == 1;
    }
    return false;
}

const char* DispName(Disp d) {
    switch (d) {
        case Disp::kOther: return "other";
        case Disp::kIdle: return "idle";
        case Disp::kListening: return "listening";
        case Disp::kSpeaking: return "speaking";
    }
    return "?";
}

std::string View(const State& s) {
    std::ostringstream o;
    o << "disp=" << DispName(s.disp) << " e=" << s.e << " chan=" << s.chan_e << " gspk=" << s.gspk
      << " grev=" << s.grev << " req=" << s.req << " mode=" << (s.mode == Mode::kAutoStop ? "auto" : "manual")
      << " wait=" << s.wait << " mic=" << s.mic;
    return o.str();
}

std::string Expected(const cJSON* x, bool* ok) {
    uint32_t e, chan, grev, req;
    bool gspk, wait, mic;
    const std::string disp = Str(x, "disp"), mode = Str(x, "mode");
    *ok = Uint(x, "e", &e) && Uint(x, "chan", &chan) && Bool(x, "gspk", &gspk) && Uint(x, "grev", &grev) &&
          Uint(x, "req", &req) && Bool(x, "wait", &wait) && Bool(x, "mic", &mic) && !disp.empty() &&
          (mode == "manual" || mode == "auto") && cJSON_GetArraySize(x) == 9;
    std::ostringstream o;
    o << "disp=" << disp << " e=" << e << " chan=" << chan << " gspk=" << gspk << " grev=" << grev
      << " req=" << req << " mode=" << mode << " wait=" << wait << " mic=" << mic;
    return o.str();
}

}  // namespace

TEST(UiModelTraces, EveryTraceMatchesTheModel) {
    const std::set<Key> manifest = ReadManifest();
    ASSERT_FALSE(manifest.empty()) << "no traces listed in " << MODEL_TRACES_DIR << "/MANIFEST";
    std::set<std::string> files;
    for (const auto& k : manifest) files.insert(std::get<0>(k));
    std::set<Key> ran;
    size_t steps_run = 0;
    for (const auto& file : files) {
        cJSON* doc = cJSON_Parse(ReadFile(std::string(MODEL_TRACES_DIR) + "/" + file).c_str());
        ASSERT_NE(doc, nullptr) << file;
        uint32_t version = 0;
        ASSERT_TRUE(Uint(doc, "version", &version) && version == 1) << file;
        const cJSON* t = nullptr;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(doc, "traces")) {
            const std::string name = Str(t, "name");
            ASSERT_FALSE(name.empty()) << file;
            State s;
            bool mic = false;  // the mic as the effects left it (Codex review 131 Important 2)
            int idx = 0;
            const cJSON* st = nullptr;
            cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(t, "steps")) {
                SCOPED_TRACE(file + " / " + name + " / step " + std::to_string(idx++));
                Event ev;
                ASSERT_TRUE(ParseEvent(cJSON_GetObjectItemCaseSensitive(st, "event"), &ev)) << "bad event";
                const StepResult r = Step(s, ev);
                s = r.state;
                for (const Action& a : r.actions) {
                    if (a.kind == ActKind::kMicOn) mic = true;
                    if (a.kind == ActKind::kMicOff) mic = false;
                }
                ASSERT_EQ(mic, s.mic) << "the effects left the mic " << (mic ? "open" : "closed");
                bool ok = false;
                const std::string want = Expected(cJSON_GetObjectItemCaseSensitive(st, "expect"), &ok);
                ASSERT_TRUE(ok) << "malformed expect";
                ASSERT_EQ(View(s), want);
                steps_run++;
            }
            ASSERT_GT(idx, 1) << name << ": no model step";
            ASSERT_TRUE(ran.insert({file, name, idx}).second) << "duplicate " << name;
        }
        cJSON_Delete(doc);
    }
    EXPECT_EQ(ran, manifest);
    EXPECT_GT(steps_run, 1000u);
}
