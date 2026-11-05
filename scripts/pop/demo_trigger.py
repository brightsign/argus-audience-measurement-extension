#!/usr/bin/env python3
"""Trigger the three Proof of Play demonstration sessions through the real scheduling API (HOST tool, standard library only).

  demo_trigger.py --api http://PLAYER:20300 correct     [--a ad-a] [--b ad-b] [--backend resilience_grpc|brightscript_screenshot] [--no-wait]
  demo_trigger.py --api ...               wrong
  demo_trigger.py --api ...               interrupted   [--yes]       (asks before it stops the playing advertisement)

correct      expected A, A plays                        -> expected verdict: Playback corroborated
wrong        expected A, B plays                        -> expected verdict: Mismatch
interrupted  expected A, A stopped after about 10 s     -> expected verdict: Incomplete

This is exactly what the dashboard's "Demonstration" buttons do: POST /api/pop/sessions with the test hooks. It needs visual_memory.pop.allow_test_jobs
(an explicit local test mode) and registered advertisements; only registered media ids can be played, never a free path. Nothing is fabricated: the
player (pop_autorun.brs) really plays the media and writes the events, and the verdict comes from the verification engine. The script then waits for the
verdict and prints it with the evidence and the similarity scores; all of it is also visible on the dashboard's Proof of Play page.
"""
import argparse, json, sys, time, urllib.error, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("--api", required=True); ap.add_argument("scenario", choices=["correct", "wrong", "interrupted"])
ap.add_argument("--a", default="ad-a"); ap.add_argument("--b", default="ad-b"); ap.add_argument("--backend", default=None)
ap.add_argument("--no-wait", action="store_true"); ap.add_argument("--yes", action="store_true", help="do not ask before an interrupted play")
ap.add_argument("--timeout", type=float, default=0, help="seconds to wait for the verdict (default: slot + 90)")
a = ap.parse_args()
API = a.api.rstrip("/")


def call(method, path, body=None):
    req = urllib.request.Request(API + path, method=method, data=json.dumps(body).encode() if body is not None else None, headers={"Content-Type": "application/json", "Origin": API})
    try:
        with urllib.request.urlopen(req, timeout=30) as r: return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try: return e.code, json.loads(e.read())
        except Exception: return e.code, {}


st, status = call("GET", "/api/pop/status")
if st != 200 or not status.get("enabled"): sys.exit("Proof of Play is not enabled on the player")
if not status.get("allow_test_jobs"): sys.exit("local test mode is off: set visual_memory.pop.allow_test_jobs=true (explicit demo/test configuration)")
media = {m["media_id"]: m for m in call("GET", "/api/pop/media")[1].get("media", [])}
for mid in (a.a, a.b):
    if mid not in media: sys.exit("advertisement '%s' is not registered (registered: %s)" % (mid, ", ".join(media) or "none"))
slot = media[a.a].get("slot_s") or 30
offs = [3, 15, 27] if slot >= 30 else [round(f * slot, 1) for f in (0.1, 0.5, 0.9)]
interrupt = 10 if slot >= 15 else max(1, round(slot * 0.7))
body = {"expected_media_id": a.a, "capture_offsets_s": offs}
if a.backend: body["capture_backend"] = a.backend
if a.scenario == "correct": body["play_media_id"] = a.a
elif a.scenario == "wrong": body["play_media_id"] = a.b
else:
    body["play_media_id"] = a.a; body["interrupt_at_s"] = interrupt
    if not a.yes and input("This starts '%s' on the player and STOPS it after about %s s. Continue? [y/N] " % (a.a, interrupt)).strip().lower() not in ("y", "yes"): sys.exit("cancelled")
expect = {"correct": "playback_corroborated", "wrong": "mismatch", "interrupted": "incomplete"}[a.scenario]
st, r = call("POST", "/api/pop/sessions", body)
if st != 201: sys.exit("could not schedule: HTTP %s %s" % (st, r.get("message") or r))
sid = r["session_id"]
print("scheduled %s  backend=%s  evidence offsets=%s s  (expected verdict: %s)" % (sid, r["capture_backend"], offs, expect))
print("follow it live on the dashboard: %s/#pop" % API.replace(":20310", ":20300"))
if a.no_wait: sys.exit(0)
deadline = time.time() + (a.timeout or slot + 90)
d = {}
while time.time() < deadline:
    st, d = call("GET", "/api/pop/sessions/" + sid)
    if st == 200 and d.get("verification"): break
    time.sleep(2)
else:
    sys.exit("no verdict within the timeout: status %s. Is the PoP autorun running on the player (it writes the playback events)? events ingested: %s" % (d.get("status"), (status.get("adapter") or {}).get("events_ingested")))
vd = d.get("verification_detail") or {}
print("\nverdict: %s (%s)\nreason:  %s\nplayed %s ms of %s ms, ended: %s, player reported %s" % (d["status_label"], d["status"], d["reason"], d["actual_play_ms"], d["slot_ms"], d.get("end_reason"), d.get("played_media_id")))
sc = {f["evidence_id"]: f for f in vd.get("frames", [])}
for e in d.get("evidence", []):
    f = sc.get(e["evidence_id"], {})
    print("  #%d  %-10s %-22s at %5.1f s  sim %s  ref@%s ms  uncertainty %s  %s" % (e["idx"], e["status"], e.get("capture_method", ""), e["actual_offset_ms"] / 1000.0,
          "%.3f" % f["sim_expected"] if "sim_expected" in f else "-", f.get("best_ref_offset_ms", "-"), e.get("time_uncertainty_ms"), e.get("failure_reason") or ""))
print("\nverdict %s the expectation (%s)" % ("MATCHES" if d["status"] == expect else "DIFFERS FROM", expect))
sys.exit(0 if d["status"] == expect else 3)
