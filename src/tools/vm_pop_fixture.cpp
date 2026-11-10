// SPDX-License-Identifier: MIT
// vm_pop_fixture: DEV/TEST tool. Writes sessions, Weston/VOP2-style history frames and verification checkpoints into a Proof-of-Play store
// (pop.db schema 2) through the real PopStore API, from JPEG files, so API/UI tests have display evidence without a player.
//
//   vm_pop_fixture --root DIR \
//       --session SID:EXPECTED:PLAYED:T0_MS \
//       --hist    SID:K:UTC_MS:FILE.jpg[:EXPECTED[:METHOD]] \
//       --ckpt    SID:IDX:UTC_MS:FILE.jpg[:METHOD]
//
// Never used on a player. Not packaged.
#include "visualmemory/mock_embedding_provider.h"
#include "visualmemory/pop.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace visualmemory;

static std::vector<std::string> split(const std::string& s, char c) { std::vector<std::string> o; std::stringstream ss(s); std::string t; while (std::getline(ss, t, c)) o.push_back(t); return o; }
static std::vector<uint8_t> slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }

int main(int argc, char** argv) {
    std::string root; std::vector<std::string> sessions, hists, ckpts;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i]; auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--root") root = next(); else if (a == "--session") sessions.push_back(next()); else if (a == "--hist") hists.push_back(next()); else if (a == "--ckpt") ckpts.push_back(next());
        else { std::fprintf(stderr, "usage: vm_pop_fixture --root DIR --session SID:EXP:PLAYED:T0 --hist SID:K:UTC:FILE[:EXP[:METHOD]] --ckpt SID:IDX:UTC:FILE[:METHOD]\n"); return 2; }
    }
    if (root.empty()) return 2;
    PopConfig pc; pc.root_directory = root; PopStore st(pc); std::string e;
    if (!st.open(e) || !st.history_open(e)) { std::fprintf(stderr, "open: %s\n", e.c_str()); return 1; }
    MockEmbeddingProvider prov(64);
    for (const auto& s : sessions) {
        const auto f = split(s, ':'); if (f.size() < 4) return 2;
        PopSession ps; ps.session_id = f[0]; ps.expected_media_id = f[1]; ps.slot_ms = 30000; ps.captures_requested = 3;
        if (!st.schedule_session(ps, e)) { std::fprintf(stderr, "session %s: %s\n", f[0].c_str(), e.c_str()); return 1; }
        const int64_t t0 = std::atoll(f[3].c_str());
        auto ev = [&](const std::string& type, int64_t dt, const std::string& reason = "", int64_t actual = -1, const std::string& played = "") {
            PopEventIn x; x.session_id = f[0]; x.type = type; x.utc_ms = t0 + dt; x.mono_ms = dt; x.reason = reason; x.actual_play_ms = actual; x.played_media_id = played;
            if (!st.apply_event(x, e)) { std::fprintf(stderr, "event %s: %s\n", type.c_str(), e.c_str()); std::exit(1); }
        };
        ev("session_scheduled_run", 0, "", -1, f[2]); ev("play_requested", 10); ev("play_started", 300); ev("play_stopped", 30300, "slot_end"); ev("session_end", 30310, "slot_end", 30100);
    }
    HistoryLimits lim;
    for (const auto& h : hists) {
        const auto f = split(h, ':'); if (f.size() < 4) return 2;
        const int k = std::atoi(f[1].c_str());
        HistoryFrameIn in; in.session_id = f[0]; in.sec_idx = k; in.target_offset_ms = 500 + k * 1000; in.actual_offset_ms = 520 + k * 1000; in.utc_ms = std::atoll(f[2].c_str()); in.mono_ms = 1000 + k * 1000;
        in.uncertainty_ms = 252; in.expected_media_id = f.size() > 4 ? f[4] : "ad-a"; in.capture_method = f.size() > 5 ? f[5] : "resilience_grpc"; in.provenance = "weston_vop2_writeback p7"; in.jpeg = slurp(f[3]);
        HistoryFrame out; if (!st.add_history_frame(in, lim, out, e)) { std::fprintf(stderr, "hist %s: %s\n", h.c_str(), e.c_str()); return 1; }
        std::printf("history %s %d -> frame_id %lld\n", f[0].c_str(), k, static_cast<long long>(out.frame_id));
    }
    for (const auto& c : ckpts) {
        const auto f = split(c, ':'); if (f.size() < 4) return 2;
        const int idx = std::atoi(f[1].c_str());
        EvidenceIn in; in.session_id = f[0]; in.idx = idx; in.target_offset_ms = idx * 5000; in.actual_offset_ms = idx * 5000 + 7; in.utc_ms = std::atoll(f[2].c_str());
        in.capture_method = f.size() > 4 ? f[4] : "resilience_grpc"; in.provenance = "weston_vop2_writeback p7"; in.uncertainty_ms = 275; in.jpeg = slurp(f[3]);
        Evidence out; if (!st.add_evidence(prov, in, out, e)) { std::fprintf(stderr, "ckpt %s: %s\n", c.c_str(), e.c_str()); return 1; }
        std::printf("checkpoint %s %d -> evidence_id %lld\n", f[0].c_str(), idx, static_cast<long long>(out.evidence_id));
    }
    return 0;
}
