#!/usr/bin/env python3
"""Side-by-side comparison of Proof of Play sessions captured with different backends (HOST tool, read-only, uses only the HTTP API).

  compare_pop_backends.py --api http://PLAYER:20300 --session shot-a --session grpc-a [--session ...] [--json out.json]

For every session: verdict and reason, then per evidence frame: capture method, similarity to the expected media and the matched reference offset (from the
stored verification details), JPEG size and geometry, acquisition latency (took_ms), reported time uncertainty (a bound, NOT a measurement), offset error
(actual vs target), and failed / missing frames. Ends with a per-backend summary. CPU / memory / Argus FPS / NPU are measured separately with
scripts/display/perf_ab.py around each run.
"""
import argparse, json, statistics, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("--api", required=True); ap.add_argument("--session", action="append", required=True); ap.add_argument("--json")
a = ap.parse_args()


def get(path):
    with urllib.request.urlopen(a.api.rstrip("/") + path, timeout=15) as r:
        return json.loads(r.read())


rows, summary = [], {}
for sid in a.session:
    d = get("/api/pop/sessions/" + sid)
    v = d.get("verification") or {}
    det = v.get("details") if isinstance(v.get("details"), dict) else {}
    frames = {f.get("evidence_id"): f for f in (det.get("frames") or [])}
    print("\n== %s: %s  (%s)" % (sid, d.get("status"), (d.get("reason") or "")[:110]))
    print("   expected %s  played %s  play %s ms  end %s" % (d.get("expected_media_id"), d.get("played_media_id"), d.get("actual_play_ms"), d.get("end_reason")))
    print("   idx  method                  status  sim    ref_off  bytes  size     took  unc    offset err")
    for e in d.get("evidence", []):
        f = frames.get(e["evidence_id"], {})
        sim = f.get("sim_expected")
        ro = f.get("expected_ref_offset_ms", f.get("best_ref_offset_ms"))
        err = e["actual_offset_ms"] - e["target_offset_ms"] if e["status"] == "ok" else None
        unc = e.get("time_uncertainty_ms")
        print("   %3d  %-22s  %-6s  %-5s  %-7s  %-6s %-8s %-5s %-6s %s" % (
            e["idx"], e.get("capture_method", ""), e["status"], "" if sim is None else "%.3f" % sim, "" if ro is None else ro, e.get("bytes", ""),
            "%sx%s" % (e.get("width"), e.get("height")), e.get("took_ms", ""), "-" if unc is None else unc, "" if err is None else "%+d ms" % err))
        if e["status"] != "ok":
            print("        failed:", e.get("failure_reason"))
        rows.append({"session": sid, "idx": e["idx"], "method": e.get("capture_method"), "status": e["status"], "sim": sim, "bytes": e.get("bytes"),
                     "took_ms": e.get("took_ms"), "unc": unc, "err": err})
    ev = d.get("evidence", [])
    summary[sid] = {"status": d.get("status"), "frames": len(ev), "failed": sum(1 for e in ev if e["status"] != "ok"), "min_sim": v.get("min_sim_expected"),
                    "mean_sim": v.get("mean_sim_expected")}
print("\n== summary by backend")
by = {}
for r in rows:
    by.setdefault(r["method"], []).append(r)
for m, rs in by.items():
    ok = [r for r in rs if r["status"] == "ok"]
    sims = [r["sim"] for r in ok if r["sim"] is not None]
    print("   %-24s frames %d  failed %d  sim median %s  jpeg bytes median %s  took_ms median %s  offset err |max| %s ms  uncertainty bound %s" % (
        m, len(rs), len(rs) - len(ok), "-" if not sims else "%.3f" % statistics.median(sims), "-" if not ok else int(statistics.median(r["bytes"] for r in ok)),
        "-" if not ok else statistics.median(r["took_ms"] for r in ok), "-" if not ok else max(abs(r["err"]) for r in ok),
        sorted({r["unc"] for r in ok if r["unc"] is not None}) or "not reported"))
if a.json:
    json.dump({"rows": rows, "sessions": summary}, open(a.json, "w"), indent=1)
