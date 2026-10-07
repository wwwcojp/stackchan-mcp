// StackChan FW-A2 §7.1-1: run the K151-DeviceLink contract test vectors (copied from the
// stackchan-works repo into contract_vectors/, listed in MANIFEST with the step count) through
// the gate core. Fails when no vector ran, when the executed (file, case, steps) set differs
// from MANIFEST (a reader that stops early fails too), or on any unknown event / malformed step.
#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "cJSON.h"
#include "playback_gate_core.h"

namespace g = stackchan::gate;

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

using Key = std::tuple<std::string, std::string, int>;  // (file, case name, steps run)

std::set<Key> ReadManifest() {
    std::set<Key> out;
    std::istringstream in(ReadFile(std::string(CONTRACT_VECTORS_DIR) + "/MANIFEST"));
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

bool GetUint(const cJSON* obj, const char* key, uint32_t* out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble != static_cast<double>(v->valueint))
        return false;
    *out = static_cast<uint32_t>(v->valueint);
    return true;
}

bool GetBool(const cJSON* obj, const char* key, bool* out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsBool(v)) return false;
    *out = cJSON_IsTrue(v);
    return true;
}

// The contract view of the gate state (README of the vectors: "state")
std::string ViewOf(const g::State& s) {
    std::ostringstream o;
    o << "bound=" << (s.bound_e != 0) << " current_gen=" << s.current_gen
      << " aborted_gen=" << s.aborted_gen << " dev_abort_seq=" << s.dev_abort_seq
      << " accepting=" << s.accepting << " speaking=" << s.speaking;
    return o.str();
}

std::string ExpectedView(const cJSON* st, bool* ok) {
    bool bound, accepting, speaking;
    uint32_t cur, ab, seq;
    *ok = GetBool(st, "bound", &bound) && GetUint(st, "current_gen", &cur) &&
          GetUint(st, "aborted_gen", &ab) && GetUint(st, "dev_abort_seq", &seq) &&
          GetBool(st, "accepting", &accepting) && GetBool(st, "speaking", &speaking) &&
          cJSON_GetArraySize(st) == 6;
    std::ostringstream o;
    o << "bound=" << bound << " current_gen=" << cur << " aborted_gen=" << ab
      << " dev_abort_seq=" << seq << " accepting=" << accepting << " speaking=" << speaking;
    return o.str();
}

std::string SentOf(const g::Sent& s) {
    std::ostringstream o;
    if (s.kind == g::SentKind::kDone)
        o << "done gen=" << s.gen << " result=" << (s.already ? "already" : "stopped");
    else if (s.kind == g::SentKind::kDeviceAbort)
        o << "abort gen=" << s.gen << " dev_abort_seq=" << s.dev_abort_seq;
    return o.str();
}

std::string ExpectedSent(const cJSON* sent, bool* ok) {
    *ok = cJSON_IsArray(sent) && cJSON_GetArraySize(sent) <= 1;
    if (!*ok || cJSON_GetArraySize(sent) == 0) return "";
    const cJSON* m = cJSON_GetArrayItem(sent, 0);
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(m, "type");
    const cJSON* state = cJSON_GetObjectItemCaseSensitive(m, "state");
    uint32_t gen;
    std::ostringstream o;
    if (!cJSON_IsString(type) || std::string(type->valuestring) != "abort" || !GetUint(m, "gen", &gen)) {
        *ok = false;
        return "";
    }
    if (cJSON_IsString(state) && std::string(state->valuestring) == "done") {
        const cJSON* result = cJSON_GetObjectItemCaseSensitive(m, "result");
        *ok = cJSON_IsString(result) && cJSON_GetArraySize(m) == 4;
        o << "done gen=" << gen << " result=" << (*ok ? result->valuestring : "?");
    } else {
        uint32_t seq;
        *ok = state == nullptr && GetUint(m, "dev_abort_seq", &seq) && cJSON_GetArraySize(m) == 3;
        o << "abort gen=" << gen << " dev_abort_seq=" << (*ok ? seq : 0);
    }
    return o.str();
}

// One step through the core. Returns false on an unknown event / malformed arguments.
bool Apply(const cJSON* step, g::State* s, g::Result* r) {
    const cJSON* ev = cJSON_GetObjectItemCaseSensitive(step, "event");
    if (!cJSON_IsString(ev)) return false;
    const std::string name = ev->valuestring;
    const uint32_t e = s->bound_e;
    uint32_t gen = 0, a = 0, k = 0;
    if (name == "bind") {
        if (cJSON_GetArraySize(step) != 2) return false;
        *r = g::Bind(*s, s->bound_e != 0 ? s->bound_e : s->last_ended_e + 1);
    } else if (name == "tts_start") {
        if (!GetUint(step, "gen", &gen) || !GetUint(step, "aborted_gen", &a) ||
            !GetUint(step, "dev_abort_seen", &k) || cJSON_GetArraySize(step) != 5)
            return false;
        *r = g::OnTtsStart(*s, e, gen, a, k);
    } else if (name == "tts_stop") {
        if (!GetUint(step, "gen", &gen) || cJSON_GetArraySize(step) != 3) return false;
        *r = g::OnTtsStop(*s, e, gen);
    } else if (name == "abort") {
        if (!GetUint(step, "gen", &gen) || cJSON_GetArraySize(step) != 3) return false;
        *r = g::OnAbort(*s, e, gen);
    } else if (name == "server_audio") {
        if (cJSON_GetArraySize(step) != 2) return false;
        *r = g::OnServerAudio(*s, e);
    } else if (name == "touch") {
        if (cJSON_GetArraySize(step) != 2) return false;
        *r = g::OnTouch(*s, e);
    } else if (name == "pair_end") {
        if (cJSON_GetArraySize(step) != 2) return false;
        *r = g::Unbind(*s, e);
    } else {
        return false;
    }
    *s = r->state;
    if (r->outcome == g::Outcome::kEnded) {
        // The manager task ends the pair (K1): Unbind. The vectors expect the state after it.
        *s = g::Unbind(*s, s->bound_e).state;
    }
    return true;
}

}  // namespace

TEST(PlaybackGateVectors, EveryManifestCaseRunsAndMatches) {
    const std::set<Key> manifest = ReadManifest();
    ASSERT_FALSE(manifest.empty()) << "no vectors listed in " << CONTRACT_VECTORS_DIR << "/MANIFEST";
    std::set<std::string> files;
    for (const auto& k : manifest) files.insert(std::get<0>(k));

    std::set<Key> ran;
    for (const auto& file : files) {
        const std::string text = ReadFile(std::string(CONTRACT_VECTORS_DIR) + "/" + file);
        cJSON* doc = cJSON_Parse(text.c_str());
        ASSERT_NE(doc, nullptr) << file << ": not JSON";
        uint32_t version = 0;
        ASSERT_TRUE(GetUint(doc, "version", &version) && version == 1) << file << ": version";
        const cJSON* cases = cJSON_GetObjectItemCaseSensitive(doc, "cases");
        ASSERT_TRUE(cJSON_IsArray(cases)) << file;
        const cJSON* c = nullptr;
        cJSON_ArrayForEach(c, cases) {
            const cJSON* nm = cJSON_GetObjectItemCaseSensitive(c, "name");
            ASSERT_TRUE(cJSON_IsString(nm)) << file;
            const std::string cname = nm->valuestring;
            g::State s;
            int idx = 0;
            const cJSON* step = nullptr;
            cJSON_ArrayForEach(step, cJSON_GetObjectItemCaseSensitive(c, "steps")) {
                SCOPED_TRACE(file + " / " + cname + " / step " + std::to_string(idx++));
                const cJSON* exp = cJSON_GetObjectItemCaseSensitive(step, "expect");
                ASSERT_TRUE(cJSON_IsObject(exp) && cJSON_GetArraySize(exp) == 4);
                g::Result r;
                ASSERT_TRUE(Apply(step, &s, &r)) << "unknown event or malformed step";
                const cJSON* outcome = cJSON_GetObjectItemCaseSensitive(exp, "outcome");
                ASSERT_TRUE(cJSON_IsString(outcome));
                EXPECT_STREQ(g::OutcomeName(r.outcome), outcome->valuestring);
                bool stopped = false;
                ASSERT_TRUE(GetBool(exp, "stopped", &stopped));
                EXPECT_EQ(r.stopped, stopped);
                bool ok = false;
                const std::string sent = ExpectedSent(cJSON_GetObjectItemCaseSensitive(exp, "sent"), &ok);
                ASSERT_TRUE(ok) << "malformed sent";
                EXPECT_EQ(SentOf(r.sent), sent);
                const std::string view = ExpectedView(cJSON_GetObjectItemCaseSensitive(exp, "state"), &ok);
                ASSERT_TRUE(ok) << "malformed state";
                EXPECT_EQ(ViewOf(s), view);
            }
            ASSERT_GT(idx, 0) << file << " / " << cname << ": no steps";
            ASSERT_TRUE(ran.insert({file, cname, idx}).second) << file << ": duplicate " << cname;
        }
        cJSON_Delete(doc);
    }
    EXPECT_EQ(ran, manifest) << "executed cases differ from MANIFEST";
}
