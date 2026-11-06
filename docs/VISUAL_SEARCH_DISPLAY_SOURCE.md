# Visual Search source: BrightSign Display (Weston/VOP2) — Phase 1

Status: implemented and tested on the host. **Not installed on the XT5.** Phase 2 (1 FPS persistence) is not implemented; see section 7.

## 1. What D5 stores today
- Table `display_capture_evidence` in the PoP database (`<pop.storage_root>/pop.db`, schema 2): one row per capture with `session_id`, `idx`, `utc_ms` (capture time), `actual_offset_ms` (into the play), `capture_method` (`resilience_grpc` = Weston/VOP2 frames, `brightscript_screenshot`), `provenance`, `uncertainty_ms`, `path` (JPEG, hourly partition `pop/evidence/YYYY/MM/DD/HH/<source>/`), `image_available`, `model_id` and `embedding` (1280-d float32 BLOB, L2-normalised, MobileNetV2 FP16, same decoder and preprocessing as the references).
- The JPEG is the frame the bridge received (stored without re-encoding). Retention removes the JPEG only; the row and the vector stay, so old evidence remains searchable and shows "JPEG expired".
- Playback timestamps: `playback_sessions` and `playback_events` (and the verification in `playback_verifications`).

## 2. sqlite-vec or a separate index?
**Neither is needed for Phase 1.** `pop.db` is a plain SQLite file opened without sqlite-vec, and its vectors are BLOB columns, not a `vec0` table. The Visual Memory library itself documents that both its backends (`vec0` and BLOB) are exact brute-force scans, so an exact dot-product scan over the evidence BLOBs is the same algorithm. At current volume (53 display frames on the XT5) a scan is far below a millisecond per candidate batch (host: 2.5 ms for 6 rows including the model call with the mock provider). A `vec0` table would add a schema change, a second copy of every vector and a dependency in the PoP database for no algorithmic gain. Revisit if evidence grows beyond roughly 10^5 rows (Phase 2 at 1 FPS reaches that in about a day of continuous play; see section 7).

## 3. Schema and index changes
**None.** No table, column, index or `user_version` change; the schema-1 `pop.db` is never opened for writing by search and is never migrated (a schema-1 file has no `capture_method` column: a Weston/VOP2 search finds zero candidates there, a screenshot search reads its rows read-only). Existing `ix_pop_evd_time` serves the time window.

Code changes: `PopStore::search_evidence`, `evidence_coverage`, `get_evidence` (libs/visual-memory), `PopService::search_display` (src/tools/pop_service.cpp), two routes in `vm_api_server`, one allow-listed POST route in the Go proxy, the dashboard (`display_search.js`, small hooks in `vm.js`, `pop.js`, `vm.css`, `index.html`).

## 4. Behaviour
- Source selector on Visual Search: **Audience Camera** (unchanged, default) or **BrightSign Display (Weston/VOP2)**. Deep link `#search?source=display`.
- The reference JPEG (≤ 8 MiB) is embedded in memory by the same provider and preprocessing as D5 (the PoP runner shares the NPU single-flight with camera search); it is never stored.
- Only rows of `display_capture_evidence` with `status='ok'`, `embedded=1`, the query's model and (default) `capture_method='resilience_grpc'` are candidates. Camera frames are in a different database and are never mixed in.
- Minimum similarity defaults to the engine's existing `match_sim` (0.80). Nothing was tuned; it can be overridden per search and the page shows which value was used.
- Each hit: similarity, capture time (and UTC), seconds into the play, advertisement id/name (played, else expected), expected id, session id and verdict, capture method, provenance, time uncertainty, image URL, and a link `#pop?session=<id>` that opens the D5 session detail. Reference and stored frame are shown side by side.
- No hit at or above the minimum: **"No matching stored evidence"**, with the text "this does not show that the content was never played", the number of stored frames searched, and the closest stored frame (marked as below the minimum).

API (via the dashboard proxy; same-origin POST only):
- `GET /api/pop/search/display?method=resilience_grpc|brightscript_screenshot|any` — coverage: `evidence_rows`, `searchable_rows`, `images_retained`, `sessions`, `oldest_utc_ms`, `newest_utc_ms`, `schema_v2`, `min_similarity_default`.
- `POST /api/pop/search/display?top_k=1..50&min_similarity=&start=&end=&media_id=&method=&require_image=1` with a JPEG body.

## 5. Tests (host)
- `libs/visual-memory/tests/test_pop.cpp`, suite `PopSearch` (6 tests): ranking and session/time fields, no-match with closest, time/advertisement/method/image filters and retention, empty store / failed captures / other model, argument validation, cancellation. Full visualmemory suite: 108/108.
- `tests/browser/display_search_cdp.py` (real processes: fake OS gRPC service, bridge, `vm_api_server`, Go proxy, headless Chrome; two real D5 sessions are run so the searched evidence was captured and persisted by D5): 48/48. Covers API contract, validation, cross-origin refusal, "search writes nothing", the page flow, side-by-side images, the session link, no-match.
- Regression: `pop_demo_cdp.py` 43/43, Go proxy tests, `tests/js` 12/12. Host tests use the mock embedding provider; real-RKNN scores must be taken on hardware (section 6).

## 6. Historical coverage on the dev XT5 (read-only GETs, 2026-11-06)
- 23 sessions, 62 evidence rows, 56 `ok`.
- Weston/VOP2 (`resilience_grpc`): 53 frames from 19 sessions, captured 07:46–09:59 local on 2026-11-06, all 53 JPEGs still retained. Ad A played in 35, Ad B in 18 (wrong-ad tests).
- BrightScript screenshots: 3 frames, 1 session. These are excluded by default (`method=any` includes them).
- Coverage is therefore only the moments of PoP sessions (3 frames per 30 s slot). Nothing outside those sessions exists to search, and the first/last of the three frames are at 3 s and 27 s.
- The XT5 currently runs the older package; it does not have the new endpoints.

## 7. Hardware deployment plan (needs your approval; nothing done)
1. Package: copy `dashboard/{display_search.js,vm.js,pop.js,vm.css,index.html}` into `install/RK3588/dashboard` (the packager reads that directory), build with `./package --ext-only --soc RK3588` (`ARGUS_DISPLAY_BRIDGE` set), keep the previous zips; verify hash and squashfs content.
2. Install by the same procedure as `argus-ext-1793953264.zip` (stop extension, unzip, installer, one DWS reboot). No OS, Weston or Resilience change; no schema change; configs and the schema-2 `pop.db` untouched; rollback = reinstall `argus-ext-1793953264.zip`.
3. Hardware checks: coverage endpoint matches section 6; upload a frame of Ad A and of Ad B (real RKNN) and record scores (expect the Ad A frame to find Ad A sessions, and report the actual numbers); upload an unrelated frame and confirm "No matching stored evidence"; follow the session link; confirm Argus FPS and the PoP tests are unaffected while a search runs (the search takes the NPU for one embedding, about the cost of one PoP frame).
4. Caveat to measure: an uploaded full-frame picture is not letterboxed like the 640x360 Weston frames; the references for PoP were built with that geometry for this reason. Upload references taken from the display path (or letterboxed 16:9) for best scores.

## 8. Phase 2 (not implemented): evaluation notes
See the report in the conversation: reuse of the 5 FPS feed, bounded queue, quota and retention are all feasible, but at 1 FPS a 30 s slot adds 30 frames (about 12 KB JPEG, 5 KB vector, one NPU embedding each) versus 3 today. Decide after Phase 1 is accepted on hardware.

## 9. Hardware validation on the development XT5 (2026-11-06)
Installed package `argus-ext-1793966121.zip` (SHA-256 `2479055a9f235e3e42e010cecb4d02df2882022281d33f38caed3b02552383dd`, squashfs `45940e70c5991738309cfe3afb3269e58c76c095275095b6f5c370f410ffa780`) from commits `5e067f4` + `5e1f751`; one reboot. Relative to the previously installed `argus-ext-1793953264.zip` only `vm_api_server`, `dashboard-server` and five dashboard files differ; all other binaries, libraries, models and configs are byte-identical.
* Config, autorun, gate file, schema-1 `visual-memory/pop.db` and the schema-2 `pop-grpc-demo/pop.db` were byte-identical to their pre-install backups after the reboot.
* Reference geometry: option `geometry=display` applies the validated display_letterbox transform (area-average resampler; MSE 3 to 20 against ffmpeg/Lanczos on real ad frames). The default `as_uploaded` is unchanged and the camera search is untouched.
* Real RKNN scores: Ad A references 0.97 to 0.996 (Ad A evidence only), Ad B references 0.99 (Ad B evidence only), unrelated pictures 0.61 to 0.63 (no match). Ad A frames from moments that were never captured (12 s, 40 s) give "No matching stored evidence" (closest 0.76).
* Latency: query embedding 20 ms median (decode + RKNN), exact scan of 53 to 60 rows 1.5 ms; end to end 25 to 50 ms as uploaded, 95 to 135 ms with the display transform of a 1080p JPEG (CPU, before the NPU is taken).
* Argus FPS 29.0 baseline vs 29.1 under one search per second vs 29.05 during a Proof-of-Play session with a search every 0.7 s; PoP verdicts unchanged.
* Not installed (needs a second reboot, not approved): the one-line wording added to the page note after validation (a match is an observation, not independent proof).
