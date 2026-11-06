# Using the Resilience OS rendered-frame service for Argus display preview and Proof of Play: source inspection and feasibility

Written 2026-11-05 from the OS LLD (`resilience-live-streaming-grpc-LLD.md`), the OS tree (`/home/sree/bs/bsoe/full_os_build/src/bsoe`), the Resilience
extension (`/home/sree/bs/resilience/bsoe-ai-player-resilience-service`) and the Argus code. **Nothing was changed in the OS tree or the Resilience
extension. Nothing was changed on the XT5 for this section** (one read-only console probe, see 3).

## 1. What exists

**OS side (`bsoe`, built into the image whose firmware string is `9.1.139-...-debug_sree-bsoe`; `local.conf` adds the distro feature
`resilience-rendered-frame-producer`, and the mission-image manifest lists `bs-resilience-api-daemon`, `bs-rendered-frame-producer` and grpc 1.24.3):**

| Piece | Behaviour |
|---|---|
| `bs-rendered-frame-producer` (runs as `weston`) | Captures one `weston_capture_v1` `SOURCE_WRITEBACK` (VOP2) frame, letterboxes it into **640x360**, JPEG-encodes it, writes it into a 3-slot POSIX SHM ring (`/bs-resilience-rendered-frame`, seqlock) and sends a notify datagram with health signals. Cadence 5 FPS (supervisor `--fps`, clamped to 5). |
| Interest gating | The producer only captures while the sentinel file `/run/brightsign/resilience/rendered_frame.interest` has an mtime younger than **5 s**. Consumers touch it (create if missing, bump mtime). Idle cost is about zero. |
| `bs-resilience-api-daemon` | Receives the notify, copies the JPEG out of SHM into `RenderedFrameStore`, and serves gRPC on `unix:///run/brightsign/resilience/runtime_api.sock`. No TCP. Sockets are `0660`, group `resilience`. |
| `RenderedOutputService` | `GetRenderedFrameHealth`, `CaptureRenderedFrame` (the LLD calls it `GetRenderedFrame`), `WatchRenderedFrameHealth` (**UNIMPLEMENTED**, poll only). |
| Freshness | The store reports **STALE after 3 s** without a new frame and never serves stale bytes. |
| Provenance | `SOURCE_PROVIDER_WESTON_VOP2_WRITEBACK` (= 7). The LLD says explicitly: approximate composed output, not bit-exact HDMI readback. |

**Extension side (`RenderedOutputGrpcClient`, `RenderedOutputFrameProvider`, `/v1/resilience/live.mjpeg`):** a thin synchronous client. It builds an
insecure channel on the unix socket, touches the interest file, calls `CaptureRenderedFrame` with a 700 ms deadline, and returns a `LiveFrame`
(JPEG, size, MIME, error code). `GetRenderedFrameHealth` is only used as a capability probe. The extension's REST and gRPC servers bind
`127.0.0.1` only, so nothing on the LAN can reach them.

**Argus side:** `vm_api_server` already owns the D5 event adapter (`pop_service.cpp`), the evidence store, MobileNetV2 embedding and the verification engine; the
dashboard server proxies `/api/vm/` and `/api/pop/` with a GET/POST allow-list, an origin check and a body cap.

## 2. Findings that shape the design

1. **The frame carries no timestamp.** `RenderedFrame` has JPEG, size, MIME and an error code only. The producer's per-slot `capture_monotonic_ms`
   and the frame sequence never reach gRPC. The only time information is in `RenderedFrameHealth`, and there:
   * `monotonic_timestamp_ms` is the daemon's **current time at the RPC** (not the capture time),
   * `freshness_timestamp_ms` is the daemon's **receipt time of the newest frame** (monotonic ms),
   * `sequence_number` is a **per-call counter** in the daemon, not a frame sequence.

   So a capture time has to be derived: call `Health`, then `Capture`, then `Health`; if `freshness_timestamp_ms` is identical in both health replies the
   returned JPEG is the frame received at that instant, otherwise discard it (a new frame arrived mid-call). The residual error is the producer latency
   (capture, scale, encode, notify) between the real display moment and the daemon's receipt. Its size must be **measured on the XT5**, and it is recorded
   as an uncertainty with each piece of evidence. Both sides use the kernel monotonic clock, which Argus can verify at run time by comparing the
   health `monotonic_timestamp_ms` with its own `CLOCK_MONOTONIC` (a mismatch rejects the source).
2. **Geometry.** Frames are letterboxed into 640x360. The XT5 output is 1280x800 (16:10), so the picture is pillarboxed (about 576x360 of picture inside
   black bars). The existing D5 reference frames are 640x400, stretched, built for `Screenshot()`. **References for this backend must be built
   with the same transform** or embeddings will not be comparable. Reference frames therefore get a profile (`screenshot` or `display_letterbox`).
3. **Interest gating means Argus must hold interest.** Whoever needs frames must keep touching the sentinel at least every 5 s: the preview viewers and any
   active Proof of Play session. Otherwise there are no frames and the daemon goes STALE after 3 s.
4. **gRPC is not in the Argus SDK.** `argus-build-cache/sdk` has no grpc++/protobuf; the Resilience SDK does (grpc 1.24.3). The host does (grpc 1.51.1,
   protoc 3.21). Linking gRPC into `vm_api_server` would need a different SDK. Isolating gRPC in a small **bridge process** built with the Resilience SDK
   avoids that and keeps `libvisualmemory` and `vm_api_server` free of the dependency.
5. **The OS pieces cannot be inspected from this host.** The daemon socket, SHM and interest file are under `/run` on the player and are not visible to
   BrightScript (`ListDir("/run/brightsign/resilience")` returns nothing; the BrightScript sandbox only sees the storage volumes). The extension REST is
   loopback only, and the DWS has no endpoint for it. Only a root process on the player (the Argus extension runs as root) can reach the socket. **On-XT5
   confirmation therefore needs code running on the player**, which needs a package install and reboot. This is the point where your approval is required.
6. **Image dependence.** The producer is a distro-feature, test-image component (the LLD says it is "test-image-only pending production promotion"). The
   daemon and SHM only exist on images built with it. The feature must be reported as unavailable, not as an error, on images without it.
7. **Not yet usable for playback truth.** The LLD lists `PresentationStatus` as not fully fed end to end. D5 therefore keeps its own playback events.
8. **A credential sits in a file in the Resilience repo** (`config/runtime.json`, the DWS snapshot section). It is not part of this work, but it is the DWS
   password that was shared in chat earlier, so the rotation I asked you to do also applies to that file.

## 3. What was verified on the player

* The Resilience extension is installed (`resilience 0.1.0+gb118171` in the DWS extension list).
* The extension REST ports (`20350`, `20351`) are not reachable from the LAN, as designed.
* The BrightScript debugger cannot list `/run/brightsign/resilience` or `/dev/shm`, so the daemon, socket and producer could **not** be confirmed from outside.
* No frames were retrieved, so dimensions, JPEG sizes, cadence, latency and content correctness are **unmeasured** so far.

## 4. Verdict

Feasible, with three conditions: (a) the OS image on the target has the producer enabled, (b) Argus gets a gRPC-free way to reach it (the bridge), and
(c) timing accuracy is accepted as "measured daemon-receipt time plus a recorded uncertainty", not frame-exact. The screenshot backend stays the default
and the fallback.

## 5. Planned architecture (implemented next, host-tested first)

```
OS: weston -> producer -> SHM -> daemon --gRPC/UDS--> argus_display_bridge (resilience SDK, grpc) --UDS--> vm_api_server
                                                                                                    |
                                       DisplayFrameSource (bounded ring, dedupe, freshness, interest)
                                         |-> /api/display/status, /api/display/frame.jpg  -> dashboard "Display output" panel (5 FPS, only while viewed)
                                         |-> PopService (backend = resilience_grpc): frames at the capture offsets -> evidence store -> MobileNetV2 -> verification
```

* Disabled by default (`visual_memory.display.enabled=false`, `visual_memory.pop.capture_backend="brightscript_screenshot"`).
* No frame is persisted unless Proof of Play selects it. The preview ring lives in memory and is bounded.
* The existing screenshot path is untouched and remains selectable per configuration.
