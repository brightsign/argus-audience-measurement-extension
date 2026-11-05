# XT5 presentation-output capture: what is and is not possible

Investigation of continuous frame capture of what an XT5 actually displays (BrightSignOS 9.1.139 debug build, XT1145,
output 1280x800x60p, presentation = `autorun.brs` + `roVideoPlayer` playing `meet-brightsign.mp4`). Date: 2026-11-05.
Everything below was measured or read on the device or in the local OS build tree; nothing is taken from Series 3/4
examples. The BrightDeveloper documentation index has no entry for `roMediaStreamer`, `display:` sources or display-output
encoding, so it could not answer these questions.

**Terminology.** *Original media frames* = what the decoder produces from the file or stream (what an MP4 decoder on the host
or in Argus would see). *Presentation-output frames* = what leaves the player on HDMI after scaling, graphics, HTML and
overlays. Only the second proves what a viewer could see. Proof of Play (D5) needs the second.

## 1. `roMediaStreamer` and the `display:` source: not supported on this XT5

* The component exists (`roMediaStreamer`, `roMediaStreamerEvent`; methods `SetPipeline`, `Initialise`, `Connect`, `Start`,
  `Stop`, `Reset`, `Inject`, `GetFailureReason`).
* `SetPipeline("display:...")` is accepted, then `Initialise()` fails with **"Protocol DISPLAY not supported"**, with or without
  an encoder or sink in the pipeline.
* Names the firmware recognises (failure text distinguishes them from unknown names, which give "No media streamer component
  available"): `display:` (not supported), `hdmi:` and `file:` (initialise OK as sources), `udp:` and `rtp:` (sinks only: "cannot be
  used as a source"), `rtsp:`, `hls:`, `http:` ("not supported"). `uvc:`, `v4l2:`, `screen:`, `video:`, `compositor:`, `framebuffer:`,
  `plane:`, `camera:`, `encode:`, `h264:`, `h265:`, `ts:`, `mp4:`, `rtmp:`, `srt:`, `tcp:` are unknown.
* `hdmi:,udp:<ip>:<port>` and `hdmi:,rtp:<ip>:<port>` initialise. No HDMI signal was present, so no stream was started.
* `file:SD:/meet-brightsign.mp4,udp:<host>:5004` initialises and `Start()` returns true, but **no packet reached the host in 9 s** and no
  `roMediaStreamerEvent` arrived (tested with and without `Connect()`). Not a display source in any case: it would be original media.

Conclusion: no supported `roMediaStreamer` pipeline encodes the display output on XT5/9.1.139, so there is no 720p30/1080p30 display
stream that Argus could take through RTSP/UDP from the player itself. (RTSP output from the streamer is also unsupported.)

## 2. What the display pipeline looks like (why a continuous tap is hard)

`roVideoMode.GetPerformanceMetrics()` on the running presentation returned the DRM state: the display is **Weston** (framebuffers say
"allocated by = weston"). The playing video sits on its own hardware plane (`Cluster2-win0` on `video_port0`) as an
**AFBC-compressed YUV420 DMA-BUF imported straight from the decoder** (format `YU08`, modifier `0x800000000000041`, 1920x1084), scaled by the
display controller to 1280x800 at scan-out. All graphics planes were idle. So with a full-screen video there is **no composed frame in
memory**; the picture only exists in the display controller as it scans out.

`Screenshot()` therefore has to make Weston re-compose (GPU) and read back, which fits the measured cost below. The local OS build
(`bsoe`, same 9.1.139) ships Weston 12.0.91 with only the `drm`, `gl-renderer` and `headless` backends and the `desktop-shell`/`kiosk-shell`
plugins: **no remoting, pipewire or screen-share plugin**, so there is no ready-made Weston streaming path. The image does ship
`librockchip_mpp`, `librga2`, the GStreamer `rockchipmpp` plugin (hardware encode/decode), libdrm 2.4.109, v4l2 and ffmpeg 5.1.

## 3. Decoded `roVideoPlayer` frames to a native extension: no supported API

* The registered BrightScript classes (`classes` in the debugger) contain no frame-access, raw-frame or encoder object: only
  `roVideoPlayer`, `roVideoMode` (with `Screenshot`), `roVideoInput`, `roMediaStreamer`, `roRtspStream` and the events above.
* Unsupported possibility, untested: a root native process could read the video plane's framebuffer through DRM
  (`drmModeGetPlane` / `drmModeGetFB2` need privilege to return GEM handles, then a PRIME fd and mmap), then decompress AFBC with RGA and
  encode with MPP. This would give **the decoded video as scanned out** (video plane only: no HTML, text or graphics overlays), at up to
  60 Hz, without Weston re-composition. It depends on kernel/DRM permissions and on Weston keeping that layout (any graphics over the video
  moves the picture back to GPU composition). It needs a native probe on the player, which has not been run and needs approval.

## 4. Alternatives

| Approach | What it captures | Status on XT5 |
|---|---|---|
| **HDMI-in loopback** (`hdmi:`/`roVideoInput`, input `hdmirx`) | true output frames, graphics included | The hardware has HDMI input (`hdmi input: true`; `GetHdmiInputStatus`: no device). Needs a cable from an XT5 output (or a splitter) into the XT5 input; the pipeline `hdmi:,udp:/rtp:` initialises. **Not measured: no signal source.** HDCP, resolution/refresh negotiation and the second output being a mirror of the first are unverified. |
| **External UVC HDMI-capture dongle** on USB plus an HDMI splitter | true output frames at 1080p30 (MJPEG/YUY2) | Argus already reads a V4L2 camera, so a second V4L2 source is a code change, not a platform limit. Needs hardware; not tested. |
| **Separate MP4 decoder** (Argus `input.file`, host ffmpeg) | **original** frames only | Works and is cheap, but cannot show that the screen played it, nor overlays or scaling. Useful as the reference side, not as evidence. |
| **`Screenshot()`** (current D5) | true output frames | Works, see section 5. |
| **DWS snapshot** (`POST /api/v1/snapshot/`) | true output frames | Works: 0.75-1.06 s per call including the digest handshake; returns a 120x80 thumbnail inline and keeps the full 1280x800 JPEG on the card (`remote_snapshots/`, latest only). Needs the DWS credential, so not suitable for an extension. |

## 5. Measurements on the player (presentation playing)

`Screenshot()` from a second `roVideoMode`, called while the presentation played (timed with `roTimespan`):

| Variant | Call blocks | `roScreenshotComplete` event |
|---|---|---|
| JPEG q85, 640x400 (D5 setting) | 326 / 331 ms | +1.50 s after the call |
| JPEG q60, 320x200 | 326 ms | +1.50 s |
| JPEG q85, 1280x800 (native) | 325 ms | +1.50 s |
| JPEG q85, 1920x1080 | 300 ms | +1.50 s |
| PNG, 640x400 | 330 ms | +1.50 s |
| BMP, 640x400 | 299 ms | +1.50 s |

* **Cost does not depend on size or format.** It is the compositor re-render and read-back, not scaling or encoding. Shrinking the
  capture does not help; the ceiling is about 3 captures per second per `roVideoMode` (calls block), far from 30 fps.
* `roScreenshotComplete` is the supported completion signal. It arrived a constant 1.5 s after the call, later than the file is usually
  complete, so D5's end-of-JPEG check (VM-17) can stay as the fast path with the event as a safety net.
* Argus FPS stayed 29-30 for the whole session (samples every second, NPU load 75-85 %, which is the audience model). The streamer test and
  the single-shot captures made no visible difference. A 20-capture back-to-back burst was **not** measured: the debugger line was too long
  to compile, so no result exists for it.
* **CPU, RAM and encoder/decoder utilisation were not measured.** `/api/pop/system` (the /proc reader) is off in the active config, and the
  Argus exporter does not expose CPU. `GetDecoderModes` shows one 8K "Regular" decoder, `usage_count 0` in the probe's own context.
* The earlier D5 hardware run (PoP active, captures every ~12 s, 3 per ad) saw Argus FPS 29.05 vs 29.06 baseline, `vm_api_server`
  1.3-3.7 % CPU and 30-31 MB, which is the only CPU/RAM figure for capture. It does not cover a 3 captures/second rate.

## 6. Recommendation

1. **Proof of Play does not need a continuous stream.** Three display captures per advertisement slot are enough to decide a verdict.
   Keep `Screenshot()` (documented, supported, true output frames). It cannot be made faster by shrinking the image; use fewer, well-spaced
   captures and keep the existing completion check.
2. For **true continuous output frames** the practical, supported route is hardware: an HDMI splitter into a UVC capture dongle (or the
   XT5 HDMI input if a loopback works), consumed as a second V4L2 source. Cost: hardware plus a modest Argus code change.
3. The **native DRM-plane tap** is the only software route to frame-rate access of the decoded output. It is unsupported and video-plane only.
   Worth a short feasibility probe only if hardware capture is not acceptable.
4. `roMediaStreamer` `display:` is not available, so nothing needs to be built for it.

## 7. Side effect to know about

The probes ran in the BrightScript debugger, which executes inside the autorun's `Main()` and shares its variable names. An early probe reused
`p` and `r`; the autorun then hit a runtime error, and the player restarted its software shortly afterwards (uptime reset to under a minute).
The autorun (`83076b0b...`) and `argus-config.json` were not modified and the presentation came back by itself. Future debugger probes must use
names that cannot collide with the running script, or run from a dedicated probe autorun with approval.
