# Proof of Play with OS rendered frames: XT5 hardware comparison plan (Step 4)

Status: **PREPARED, NOT EXECUTED.** Nothing described under "Hardware procedure" has been run. Nothing was installed, rebooted, reconfigured or pushed during the preparation.
Running it needs explicit approval of this exact procedure, including the autorun swap (section 3) and a new extension package (section 9).

## 1. What was prepared on the host

| Item | Where | Result |
|---|---|---|
| Display geometry validated against real XT5 frames | `scripts/pop/validate_display_geometry.py`, `docs/test_results/pop_display_prep/geometry_report.json` | Best model: video in a 1920x1080 rectangle, 1280x800 output shows its top-left part, then letterboxed to 640x360 (576x360 picture). Median MSE 17.4; every alternative (fill-crop, stretch, fit) at least 3600 |
| Reference generation from the ORIGINAL videos only | `scripts/pop/pop_reference_tools.py make-refs`, `scripts/pop/make_reference_frames.sh <video> <dir> 1 0 display_letterbox` | Manifest per media: model version, video SHA-256, ffprobe data, exact filter, JPEG quality |
| Similarity calibration (proxy) | `scripts/pop/pop_reference_tools.py calibrate`, `calibration.json` | see section 7 |
| Relative timing jitter from real probe frames | `timing_jitter_from_probe.json` | std 37 ms, extremes -192/+128 ms. This is jitter only, NOT absolute latency |
| Isolated schema-2 store | `visual_memory.pop.storage_root` (commit 5d1780a) | host e2e proves a byte-identical stand-in schema-1 `pop.db` in the VM root is untouched |
| Legacy protection | `pop.allow_schema_upgrade` (default false) | a schema-1 file opens read/write in legacy mode, byte-identical; gRPC sessions refused with 409 on it |
| Readiness, guards, 1 FPS mode | `grpc_ready_timeout_ms`, `grpc_start_guard_ms`, `grpc_end_guard_ms`, `capture_every_s` | host e2e |
| In-place restart on config change | `vm_api_server` re-exec | no root shell needed to apply config |
| Comparison tooling | `scripts/pop/compare_pop_backends.py`, `timing_calibrate.py`, `scripts/display/perf_ab.py` | host-side, API read-only |

Host results: library suite 102/102, display unit tests 14/14, PoP e2e OK, display e2e OK (see section 8 for the flake that was found and fixed in the test).

## 2. Limits that apply to every conclusion

* The embedding calibration uses the ONNX FP32 network as a PROXY for the device FP16 RKNN. Values differ slightly; the hardware run is the real calibration.
* There are no genuine frames of advertisement B yet; the host negatives are geometry-faithful simulations from the original B video.
* The absolute producer-to-API latency is NOT measured. `display.producer_latency_ms` (250) is a configured bound. Only jitter has been measured.
* Scaling of 3840x2160 media into the 1920x1080 rectangle is assumed, not verified (both reference videos must be checked, see 4.3).
* CPU/RSS of `argus_display_bridge`, `bs-rendered-frame-producer`, `bs-resilience-api-daemon` and `weston` are not visible in the currently installed `/api/pop/system` process list; the list is extended in this preparation (section 9), effective with the new package.

## 3. Preconditions and approvals required

1. Approval of this procedure.
2. Approval of ONE package install and ONE DWS reboot (section 9; the current installed package predates 5d1780a).
3. Approval to swap `autorun.brs` for the PoP autorun for the duration of the test, with the original restored afterwards. The original is SHA-256 `83076b0b...` (backup `autorun.brs.pre-d5`). Hardware PoP sessions cannot run without the PoP autorun because it is what emits the play events. The autorun file itself is not edited.
4. Not changed at any point: Telnet stays off, gRPC stays on its Unix socket, no credentials are copied, nothing is pushed.

## 4. Hardware procedure (proposed, in order)

### 4.0 Backup (read-only)
`argus-config.json`, `autorun.brs`, the installed zip, and `pop.db`, `/storage/sd/visual-memory/` listing: each copied to the host with SHA-256. Existing packages and rollback zips are kept.

### 4.1 Install
Build per `docs/RESILIENCE_DISPLAY_FRAMES.md` (copy older zips first: `./package` deletes them), copy to SD, descend to the root shell ONLY for the installer, then reboot through DWS `PUT /api/v1/control/reboot` (a `#REBOOT` in bsdrive only restarts player software and does not remount the extension volume). The root-shell descent stops the player application and the DWS: finish all DWS work first and expect the presentation to stop until the reboot.

### 4.2 Isolated configuration
Edit `argus-config.json` (backup first), `visual_memory`:
```
"storage_root": <unchanged>,
"display": {"enabled": true, "manage_bridge": true, "producer_latency_ms": 250},
"pop": {"enabled": true, "capture_backend": "resilience_grpc",
        "storage_root": "/storage/sd/pop-grpc-test",
        "events_path": "/storage/sd/pop-grpc-test/events.jsonl",
        "jobs_dir": "/storage/sd/pop-grpc-test/jobs",
        "allow_schema_upgrade": false}
```
`pop.storage_root` makes a fresh schema-2 `pop.db` under `/storage/sd/pop-grpc-test`. Verify before any session: the old `pop.db` hash is unchanged, the new one reports schema 2, `GET /api/display/status` shows `ready: true`. The service restarts in place on a config change.

### 4.3 References
Generate on the host from `meet-brightsign.mp4` (A) and `snowy_river.mp4` (B) with `make_reference_frames.sh ... display_letterbox` (1 s spacing; 0.5 s for the transition windows, see 7). Copy to `/storage/sd/pop-grpc-test/pop/refs_incoming/<id>.display_letterbox/`, build with `POST /api/pop/media/<id>/references/build?profile=display_letterbox`. Re-run `validate_display_geometry.py` with the real A and real B frames captured in 4.4 and confirm both videos (including the 3840x2160 case) match the same geometry. If either does not, stop and fix the filter before scoring.

### 4.4 Scenarios (each with a unique session id, `capture_offsets_s` such as 1,3,5 and then the dense 1 FPS variant)

| # | Scenario | Setup | Expected verdict |
|---|---|---|---|
| S1 | A expected, A played | schedule A, play A | Playback corroborated |
| S2 | A expected, B played | schedule A, play B | Mismatch (similarity to A below 0.60 or B's references win by the margin) |
| S3 | Interrupted | schedule A, stop the play before `min_play_ratio` (0.90) of the slot | Incomplete |
| S4 | Producer unavailable | the OS services are NOT stopped. Use `display.enabled:false` for one session (in-place config restart) or the `break_capture` test hook | Capture unsupported / inconclusive (failed evidence rows with reasons), never Corroborated |
| S5 | No fresh evidence | session started while `ready:false` (within the first seconds after a restart) | refused with 503 "not ready", or inconclusive; never a frame from before the play |

Each of S1-S3 is run with `brightscript_screenshot` and with `resilience_grpc` (the screenshot runs use the existing, unmodified D5 path and the production pop storage root, or a copy, per approval). At least 5 repetitions of S1 and S2 for any statement about calibration.

### 4.5 Measurements per run
* `compare_pop_backends.py`: verdict, similarity, matched reference offset, JPEG bytes/size, `took_ms`, uncertainty bound, offset error, failed/duplicate/missing evidence.
* `timing_calibrate.py`: the MEASURED end-to-end timing error (frame content time minus assumed offset) for S1, which is what replaces the 250 ms assumption. Adjust `producer_latency_ms` and the guards only from this measurement.
* `perf_ab.py` around each run: Argus FPS, NPU load, CPU and RSS of the listed processes (after the section 9 change).
* Full logs of every run are kept in a dated folder on the host.

### 4.6 1 FPS evidence evaluation
`capture_every_s: 1` on a 10 s slot, then 30 s. Measure cadence, missed frames, per-frame time error. Sampling is asynchronous and bounded by `grpc_max_dense_captures` (60); only the selected frames are stored, never the 5 FPS stream. Not enabled permanently; the 3-frame mode stays the baseline.

## 5. Expected outcomes

* S1 grpc: all frames at similarity about 0.9+ (proxy median 0.99, min 0.78), Playback corroborated, evidence `weston_vop2_writeback p7`, uncertainty bound = RPC span + 250 ms.
* S2 grpc: similarity to A about 0.5-0.62 (proxy), B's references win, Mismatch.
* S3, S4, S5 as in the table. A frame close to a start or end transition is rejected with "transition-adjacent", never counted as a positive.
* The first Health after activation is STALE/503 and is expected; the backend becomes `ready` only after a genuinely fresh frame.

## 6. Why the preparation changed the defaults

The first draft's reference filter (`scale=576:360,pad=...`) does not match the real display (full-frame stretch lowers real-vs-reference similarity to a median of 0.906 vs 0.988 with the validated geometry). The `screenshot` profile is unchanged (D5 baseline).

## 7. Threshold evaluation (proxy, host)

| Set | n | min | median | max |
|---|---|---|---|---|
| Real A frames vs A refs (positives) | 92 | 0.784 | 0.988 | 0.998 |
| Real A frames vs B refs | 92 | 0.476 | 0.517 | 0.617 |
| Simulated B frames vs A refs (negatives) | 60 | 0.601 | 0.612 | 0.623 |

* `match_sim` 0.80 would reject 1 of 92 genuine positives (min 0.784), probably a transition frame. That is why one weak frame must not decide a verdict (`min_match_fraction` 0.60 handles it).
* 62 negatives are at or above `reject_sim` 0.60 (none above 0.80): negatives are NOT cleanly below 0.60. Mismatch then relies on the second rule (another registered media matches at least `match_sim` and better than the expected one by the margin), which requires B to be registered with references. An unregistered wrong advertisement would land in the ambiguous band, not Mismatch.
* Gap between lowest positive and highest negative: 0.161 (midpoint 0.70). Do not change thresholds on this alone: genuine B frames and the FP16 network are needed. Recommendation: keep 0.80/0.60 for the hardware run, record raw similarities, then decide.

## 8. Host test notes

`tests/api/vm_display_e2e.py` had a nondeterministic check ("a frame at the very end of the play is transition-adjacent"): with a 6.2 s play the latest usable capture is 5.8 s and the window for offset 6 starts at 5.7 s, so a 100 ms slice was legitimately usable about half the time at 5 FPS. The code behaviour is right (an earlier, non-adjacent frame is valid evidence); the test now uses target 5.95 s on a 6.0 s play so the outcome is deterministic.

## 9. Required code / package modifications

Already in commit `5d1780a` and earlier (need ONE new package): `pop.storage_root`, readiness gating, start/end guards, dense capture, in-place restart, legacy-mode protection, display-geometry reference profile.

Done in this preparation (also needs the new package): the `/api/pop/system` process list now includes `argus_display_b`, `bs-rendered-fra`, `weston` (kernel `comm` is truncated to 15 characters; `bs-resilience-ap` was already listed), so CPU/RSS can be measured.

Still to do after hardware data:
1. Optional after hardware data: reference spacing 0.5 s near transitions; `reject_sim`/`match_sim`; `producer_latency_ms` and guards from the measured timing error.

## 10. Why the vm_api_server restart paused the presentation

Diagnosed earlier: the pause was caused by the console descent (Ctrl-C into the debugger stops the BrightScript application), not by the service restart. With the in-place restart a config change no longer needs a shell.

## 11. Rollback

1. Restore `argus-config.json` from the 4.0 backup; the service restarts in place. Verify SHA-256.
2. Restore the original `autorun.brs` (SHA-256 `83076b0b...`, backup `autorun.brs.pre-d5`) and restart the player software.
3. If the package must go: reinstall the previous zip (kept), then DWS reboot.
4. The isolated store `/storage/sd/pop-grpc-test` is simply left or deleted with approval; the production `pop.db` was never opened by the test configuration. Caveat: a schema-2 file cannot be downgraded; this is why the production file is never upgraded.
5. Verify afterwards: `pop.db` hash equals the 4.0 backup, `display.enabled` false, `GET /api/display/status` reports disabled, the bridge process is gone.

## 12. The D5 demonstration (what the dashboard `/#pop` now offers)

Implemented and host-tested (`tests/browser/pop_demo_cdp.py`, 37 checks, real chain: OS-service double, bridge, vm_api_server, dashboard server, Chrome; a player emulator that does what `pop_autorun.brs` does):

| Dashboard element | What it is |
|---|---|
| Advertisement registry | Ad A / Ad B, media ids, SHA-256, duration, slot, reference counts for both profiles (screenshot and Weston/VOP2 display output) |
| Demonstration (local test mode) | **Verify Correct Ad** / **Verify Wrong Ad** / **Verify Interrupted Ad** (confirmation before it stops the playing ad). They call the ordinary `POST /api/pop/sessions` with the existing test hooks (`play_media_id`, `interrupt_at_s`); only REGISTERED media ids can be played; disabled with the reason unless `visual_memory.pop.allow_test_jobs` is true (the server answers 403 otherwise). Evidence offsets 3/15/27 s on a 30 s slot |
| Live Display output | the Weston/VOP2 preview, on this page |
| Current session | expected ad, actual ad reported by the player, start time, elapsed / slot with a progress bar, evidence captured of requested, backend; 1 s refresh while a session runs; "Waiting for the player to start" until a real playback event arrives |
| Playback history | status, reason, backend, start/end, played/slot, evidence count and matches, mean similarity; filters; **Export CSV** and **Export JSON** |
| Session details | per evidence frame: the captured frame, the closest reference of the expected ad and (for a mismatch) of the better-matching other ad, similarity, matched reference offset, capture method, provenance `weston_vop2_writeback pN`, capture time, `time_uncertainty_ms`; "Why this result" (the engine's reason plus the thresholds in force); player event timeline; per-session JSON export |

CLI equivalents (same API, same hooks): `scripts/pop/demo_trigger.py --api http://PLAYER:20300 correct|wrong|interrupted`.
Provisioning: `scripts/pop/provision_demo.py prepare|apply` (reference bundle generated from the ORIGINAL videos: Ad A 121.7 s, 122 + 122 reference frames at 1 s; Ad B 15.0 s, 15 + 15; hashes `81e3cc01...` and `885b3680...`). The bundle contains frames of people and therefore stays out of Git.

### 12.1 Why the dashboard shows no playback events today (investigated, read-only)

On the XT5 (`GET /api/pop/status`, nothing modified): the adapter is running and reading `/storage/sd/pop/events.jsonl`, but that file is 0 bytes (0 lines, 0 boots, 0 events ingested); `media` 0, `sessions` 0; the store is schema 1; `allow_test_jobs` false; backend `brightscript_screenshot`.
Cause: the installed autorun is the original presentation. It plays the video but writes no D5 events, and not even the `boot` event that `scripts/pop/pop_autorun.brs` writes at start. Without that player-side reporter there is nothing to verify, and the engine correctly never reports a play. The gRPC preview alone is never treated as a playback event.
The original presentation is not modified. For the demonstration the existing D5 test autorun is used (it plays exactly the original presentation, looping `meet-brightsign.mp4`, until `SD:/pop/enabled` exists).

### 12.2 Exact changes the hardware demonstration needs (none is made yet)

1. **Package:** install `argus-ext-<new>.zip` (contains the dashboard move, the Proof of Play demo UI, the isolated storage root, readiness gate, guards, bridge). One DWS reboot after the install.
2. **Configuration** (`/storage/sd/configs/argus-config.json`, backup and SHA-256 first):
   ```
   "visual_memory": { ...unchanged...,
     "display": { "enabled": true, "manage_bridge": true },
     "pop": { "enabled": true, "storage_root": "/storage/sd/pop-grpc-demo",
              "capture_backend": "resilience_grpc", "allow_test_jobs": true } }
   ```
   `pop.storage_root` creates a NEW schema-2 `pop.db` under `/storage/sd/pop-grpc-demo`; the existing schema-1 `pop.db` in the Visual Memory root is not opened, migrated or modified (`allow_schema_upgrade` stays false). `events_path` / `jobs_dir` stay the defaults used by the autorun (`/storage/sd/pop/events.jsonl`, `/storage/sd/pop/jobs`).
3. **Files on the SD card:** `snowy_river.mp4` (15 s) next to the already present `meet-brightsign.mp4`; the reference bundle's `refs_incoming/` into `/storage/sd/pop-grpc-demo/pop/refs_incoming/`; `/storage/sd/pop/enabled` (turns job mode on).
4. **Autorun:** back up `autorun.brs` (SHA-256 recorded, `autorun.brs.pre-d5`), copy `scripts/pop/pop_autorun.brs` as `autorun.brs`, restart the player application. The original presentation is untouched as a file backup; the test autorun behaves identically while `SD:/pop/enabled` is absent.

### 12.3 Demonstration procedure (after approval)

1. `provision_demo.py apply --api http://192.168.0.165:20300 --bundle <bundle>`: registers Ad A and Ad B and builds both reference profiles; the registry on `/#pop` shows them with 122/122 and 15/15 reference frames.
2. Open `http://192.168.0.165:20300/#pop`: the registry, the live preview and the three controls are visible.
3. Verify Correct Ad, then Wrong, then Interrupted (or `demo_trigger.py`), each 30 s (interrupted ~10 s). Expected: Playback corroborated; Mismatch (similarity to A about 0.5-0.62 for B frames; B frames should match Ad B's own references closely, which is what triggers the "another ad matches better" rule; neither number has been measured on the XT5 yet, the similarities quoted are host proxies); Incomplete.
4. Open each session: frames at about 3, 15 and 27 s beside their references, scores, uncertainty and the explanation. Export CSV/JSON.
5. Compare backends: the same three scenarios with `capture_backend: brightscript_screenshot` for the comparison (`compare_pop_backends.py`, `timing_calibrate.py`, `perf_ab.py` as in section 4.5).
6. Also run S4/S5 (capture unavailable, no fresh frames) from section 4.4.

### 12.4 Rollback for the demonstration

Restore `argus-config.json` (verify SHA-256), restore the original `autorun.brs` from the backup and restart the player application, delete `/storage/sd/pop/enabled`, optionally delete `/storage/sd/pop-grpc-demo` (test store, schema 2, never merged into the production one), reinstall the previous package zip if required. The schema-1 `pop.db` and its evidence were never opened by the demonstration configuration.

### 12.5 Open limits

The 0.80/0.60 thresholds are kept for the first comparison and are not a calibration; real FP16 RKNN similarities for genuine Ad A and Ad B frames on the XT5 are the data that decides. Absolute producer latency is still unmeasured (`timing_calibrate.py` measures it on hardware; guards of 400 ms plus the 250 ms configured bound are conservative placeholders). The 30 s slot plays the first 30 s of Ad A (121.7 s video); Ad B (15 s) loops twice, so the evidence at 15 s sits on the loop boundary of B.
