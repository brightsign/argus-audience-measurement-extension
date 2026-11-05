# Display-output frames from the OS Resilience service (preview and Proof of Play)

Optional, **disabled by default**, OS-image dependent. It reuses the OS `RenderedOutputService` (weston VOP2 writeback, served over gRPC on a
local Unix socket by `bs-resilience-api-daemon`) instead of adding another capture implementation. The BrightScript `Screenshot()` backend of
Proof of Play is untouched and stays the default. Background and findings: `RESILIENCE_DISPLAY_FRAMES_FEASIBILITY.md`.

**Status: implemented and tested on the host against a test double of the OS service. First hardware probe done on the dev XT5 (2026-11-05,
`test_results/display_probe_xt5.json`):** the OS daemon and producer are present and serve genuine 640x360 JPEG frames of the presentation at ~4.7-5 FPS with
provider `WESTON_VOP2_WRITEBACK`, the interest mechanism wakes the producer within one cycle, the daemon clock equals this process's monotonic clock (0-1 ms),
and an idle producer correctly answers STALE. The display preview was then enabled on hardware (Step 3, `test_results/display_preview_xt5.json`): about 4.4-5 FPS, frame age ~0.1-0.3 s, colours and framing match a DWS snapshot of the HDMI display (PSNR 29 dB), no visible effect on Argus FPS, and about +0.35 core of system CPU while a viewer is attached (mostly outside the processes Argus can see). The Proof of Play backend has **not** been run on hardware yet.
Every number a full hardware run should produce is listed in section 7.

## 1. What it is, and what it is not

* **Is:** an approximate preview of the composed display (video plus graphics), ~5 frames per second, and an alternative source of Proof of
  Play evidence frames (`capture_method = resilience_grpc`).
* **Is not:** a bit-exact HDMI readback (the OS says so: provider `WESTON_VOP2_WRITEBACK`), nor a continuous recording. The preview ring is
  in memory and bounded; only the frames Proof of Play selects are stored.
* **Needs:** an OS image built with the producer (`resilience-rendered-frame-producer` distro feature), the daemon running, and the OS
  `grpc 1.24.3` + `libprotobuf.so.22` libraries (present in the same image).

## 2. Architecture

```
OS: weston -> bs-rendered-frame-producer -> SHM -> bs-resilience-api-daemon --gRPC/UDS--> argus_display_bridge --UDS--> vm_api_server
                                                                                              (supervised child)        |
                                       DisplayFrameSource: Health/Capture/Health, dedupe, freshness, clock + provenance checks, bounded ring
                                         |- /api/display/status, /api/display/frame.jpg  --dashboard-server (GET only)--> "Display output" panel
                                         '- PopService (capture_backend resilience_grpc): frame nearest each capture offset -> evidence store -> MobileNetV2 -> verification
```

| Component | File | Role |
|---|---|---|
| `argus_display_bridge` | `src/tools/display_grpc_bridge.cpp` | The **only** gRPC component. Forwards Health / Capture / interest-touch from a local socket to the OS service. Also has a read-only `--probe` mode. Built with the Resilience SDK (it has grpc++). |
| `BridgeClient`, wire format | `src/display/bridge_client.*`, `bridge_wire.h` | gRPC-free client in `vm_api_server`. |
| `BridgeSupervisor` | `src/display/bridge_supervisor.h` | `vm_api_server` starts and restarts the bridge (backoff), and the bridge dies with it. No change to `bsext_init`. |
| `DisplayFrameSource` | `src/display/display_frame_source.*` | Polling, validation, ring, status. No gRPC, no BrightScript, no libvisualmemory. |
| Routes | `src/tools/vm_api_server.cpp` | `GET /api/display/status`, `GET /api/display/frame.jpg[?after=<seq>]`. |
| Proxy | `dashboard-server/main.go` | `/api/display/` mounted with the existing policy: GET only, optional token, never exposes the OS socket. |
| Panel | `dashboard/display.js` | "Display output" panel in the Proof of Play view, under the banner. |
| PoP backend | `src/tools/pop_service.*`, `libs/visual-memory/src/pop_store.cpp` | Evidence selection, `capture_method` / provenance / uncertainty stored (pop.db schema v2), display-geometry references. |
| Vendored contract | `third_party/resilience-proto/` | The OS `.proto` files, unchanged. |

**Reuse of the Resilience extension:** the bridge follows `RenderedOutputGrpcClient` (insecure channel on the unix socket, per-call deadline,
interest-file touch) but is not a copy of it: the extension's `LiveFrame` drops the health metadata this feature needs (freshness and the
daemon clock), and the extension's tree is not a library. The OS-owned proto is vendored, not duplicated.

## 3. How frames are accepted (and why)

The OS frame message has no timestamp, and `GetRenderedFrameHealth` gives the daemon's current time and its receipt time of the newest frame
(`freshness_timestamp_ms`); its `sequence_number` is a per-call counter, not a frame number. So per cycle (default every 200 ms, only while
needed):

1. `Health` (before). Not available / `STALE` / `UNAVAILABLE` / wrong provenance / daemon clock more than 1.5 s from ours: nothing is stored, the state says why.
2. Same freshness as the last accepted frame: skip (no capture call, so no duplicates).
3. `Capture`, then `Health` (after). If the freshness changed, a newer frame arrived mid-call and which one we hold is unknown: **discarded** (counted as `ambiguous`).
4. The JPEG must be complete (SOI, EOI, size markers) and below 1 MiB.
5. Capture time = freshness timestamp (monotonic) converted to UTC with this process's own clock offset. It is an **upper bound** of the real display moment;
   the evidence stores an uncertainty = RPC span + `producer_latency_ms` (default 250 ms, **not measured yet**, flagged as such in the status).

A frame older than `max_age_ms` (1.2 s) is not served as current. A changed `boot_id` (daemon restart) clears the ring.

## 4. Interest gating (no idle cost)

The OS producer captures only while `/run/brightsign/resilience/rendered_frame.interest` is younger than 5 s. Argus touches it (via the bridge, every 2 s)
**only** while a dashboard viewer is requesting frames (kept for 3 s after the last request) or a Proof of Play session is active. Otherwise there are no
RPCs, no capture, no CPU. The dashboard panel requests nothing while the Proof of Play view or the browser tab is not visible.

## 5. Proof of Play with `resilience_grpc`

Select per deployment (`visual_memory.pop.capture_backend`) or per session (`capture_backend` in `POST /api/pop/sessions`).

1. The job file sent to the player has an **empty** `capture_offsets_ms`; the player application takes no screenshots and still reports playback events (unchanged).
2. At `play_started` the source is held (producer kept awake). For each offset (default 10 / 50 / 90 % of the slot, i.e. 3 / 15 / 27 s of 30 s) the frame closest to
   `play start + offset` within `-300 ms / +800 ms` is taken from the ring, once frames just after the target exist.
3. Evidence is rejected (recorded as a **failed capture** with the reason, like a failed `Screenshot()`) if no such frame exists, or its time uncertainty exceeds
   `grpc_max_uncertainty_ms`. Offsets the play never reached get no evidence (same as screenshots).
4. The JPEG is stored in the existing evidence store (hourly partitions, retention), embedded with the existing MobileNetV2 FP16 RKNN backend, and verified by
   the unchanged engine. Each row records `capture_method`, `provenance` and `time_uncertainty_ms`.
5. At `session_end` the remaining offsets are resolved, the hold is released, and the producer idles again.

Outcomes are preserved: correct ad = Playback corroborated; wrong ad = Mismatch; interrupted = Incomplete; no usable frames = Capture unsupported.

**Reference frames need the display geometry.** The OS frames are 640x360 letterboxed (the 1280x800 display becomes a 576x360 picture centred on black).
References built for screenshots (640x400, stretched) are not comparable. Build a second set:

```bash
scripts/pop/make_reference_frames.sh ad.mp4 /tmp/refs 1 30 display_letterbox        # 640x360, picture 576x360 centred
# copy to <storage_root>/pop/refs_incoming/<media_id>.display_letterbox/ , then
curl -X POST -H "Origin: http://<player>:20300" http://<player>:20300/api/pop/media/<media_id>/references/build?profile=display_letterbox
```

They live in a hidden media row `<media_id>.display_letterbox` and are used only for `resilience_grpc` evidence (never mixed with the screenshot set; if no display set
exists the screenshot references are used as a fallback). **The 576x360 transform is an assumption to confirm on the XT5** by comparing one reference with a real frame.

## 6. Configuration (all under `visual_memory`)

```json
"display": { "enabled": true, "bridge_socket": "/tmp/argus_display_bridge.sock", "poll_ms": 200, "max_age_ms": 1200, "ring_frames": 16,
             "producer_latency_ms": 250, "manage_bridge": true, "bridge_binary": "", "os_socket": "unix:///run/brightsign/resilience/runtime_api.sock",
             "interest_file": "/run/brightsign/resilience/rendered_frame.interest" },
"pop": { "capture_backend": "resilience_grpc", "grpc_window_before_ms": 300, "grpc_window_after_ms": 800, "grpc_max_uncertainty_ms": 1500 }
```

`display.enabled` defaults to `false`; `pop.capture_backend` defaults to `brightscript_screenshot`. `bridge_binary` defaults to `<extension>/argus_display_bridge`.

## 7. Tests (host) and what a hardware run must measure

| Test | Result |
|---|---|
| `tests/display/test_display_source.cpp` (14, scripted client) | freshness bracketing, duplicates, ambiguity, stale, clock mismatch, provenance, torn/oversize JPEG, restart, ring bound, nearest-frame window, interest gating |
| `tests/api/vm_display_e2e.py` (59 checks; real gRPC double, bridge, API) | disabled by default; no RPC when idle; ~5 fps preview; content / black / frozen; stale, resume, wrong provenance, clock skew, service restart, bridge kill + supervisor restart, no orphan; PoP correct / wrong / interrupted / no frames / hook; only selected frames stored |
| `tests/browser/display_panel_cdp.py` (17, headless Chrome) | panel hidden when disabled, image decoded, metadata, black / frozen / stale, polling stops when the view is left |
| `libs/visual-memory/tests/test_pop.cpp` (+5) | pop.db v1 to v2 migration, capture method and uncertainty stored, display-geometry references, unchanged outcomes |
| Go: `TestDisplayPreviewIsGetOnlyAndProxied` | proxy is GET only |

Host tests use synthetic pictures and a test double. They verify plumbing and decisions, **not** hardware. On the XT5, record: Health/Capture latency and cadence
(`argus_display_bridge --probe 100 --out DIR`), clock agreement (`clock_delta_ms`), frame sizes, whether frames show the real video and overlays, the real
producer latency (to replace the 250 ms budget), CPU / RAM of `vm_api_server` and the bridge, NPU load and Argus FPS during a session, and equivalent verdicts
for the same four scenarios on both backends.

## 8. Build, deploy, enable, disable, roll back

**Nothing below has been done on the XT5; each step that changes the player needs approval.**

1. **Build the bridge** (aarch64, Resilience SDK, needs grpc++ 1.24.3 / protobuf 3.11.4 which the OS image also ships):
   ```bash
   source /home/sree/bs/resilience/bsoe-ai-player-resilience-service/sdk/environment-setup-aarch64-oe-linux
   cmake -S tests/display -B build-bridge-xt5 -DDISPLAY_BUILD_TESTS=OFF -DCMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake -DCMAKE_BUILD_TYPE=Release
   cmake --build build-bridge-xt5 -j && ls -l build-bridge-xt5/argus_display_bridge
   ```
   It links `libgrpc++.so.1` and `libprotobuf.so.22` dynamically from the OS root file system.
2. **Build and package the extension** with Visual Memory flags (see `VISUAL_MEMORY_OPERATIONS.md`) and the bridge:
   `ARGUS_DISPLAY_BRIDGE=$PWD/build-bridge-xt5/argus_display_bridge EXTRA_CMAKE_FLAGS="-DENABLE_VISUAL_MEMORY=ON -DENABLE_VISUAL_EMBEDDINGS=ON" make build SOCS=rk3588 && ./package --ext-only --soc RK3588`.
3. **Probe first, read-only** (before enabling anything): with the package installed, run `argus_display_bridge --probe 100 --out /tmp/probe` once from a root shell
   and check: `rpc_failures 0`, `capture_available` true, provider 7, `clock_delta_ms` small, frame gaps near 200 ms. If the daemon or producer is missing, stop here.
4. **Enable:** add the `display` block (section 6) to `/storage/sd/configs/argus-config.json`, restart Argus. Open the Proof of Play tab: the "Display output" panel appears under the banner.
5. **Proof of Play:** build the display-geometry references, set `pop.capture_backend` (or pass `capture_backend` per session), run the four scenarios.
6. **Disable:** set `display.enabled` to `false` (the panel disappears, the bridge is no longer started, the OS producer idles within 5 s) and/or
   `pop.capture_backend` to `brightscript_screenshot`. No data migration is needed: pop.db v2 rows keep working, and the screenshot backend is unaffected.
7. **Roll back the package:** reinstall the previous zip with the normal procedure (the installed `argus-ext-1793873298.zip` and the older rollback zips remain on the card).
   A v2 `pop.db` is **not** readable by a v1 release (it refuses a newer schema); keep a copy of `<storage_root>/pop.db` before upgrading if a downgrade may be needed.

## 9. Known limits

* Producer latency, clock agreement and the real letterbox transform are unmeasured until a hardware run.
* One OS producer: frames are shared; viewers and sessions do not get separate captures.
* The OS service has no frame sequence or capture timestamp, so a frame whose receipt coincides with a newer one is discarded (about a few per cent at 5 fps).
* Display capture of protected (DRM) content is not verified.
* `WatchRenderedFrameHealth` is UNIMPLEMENTED in the OS, so this feature polls.
