# Final demo readiness (frozen build `argus-ext-1794251841.zip`, 2026-11-09)

Feature development is frozen. Runbook: [FINAL_STAKEHOLDER_DEMO_RUNBOOK.md](FINAL_STAKEHOLDER_DEMO_RUNBOOK.md). Results: [VISUAL_MEMORY_DISPLAY_OBJECTS_HW_RESULTS.md](VISUAL_MEMORY_DISPLAY_OBJECTS_HW_RESULTS.md).

## Frozen state (nothing on the player was changed in this step)
`hw_validation_20261109/frozen_demo_state/FROZEN_STATE.txt` lists the hashes: package `8bed0a93...8513`, squashfs `4b553933...`, `vm_api_server` `b9236160...`, dashboard files served by the player identical to the package (11/11), active config `329c58ec...` (= original + the display block), autorun `394d7be1...` (= original), read-only copies of the live `objects.db` (v3, integrity ok, camera 2,295 auto + 3 manual, display 404 auto + 4 manual, 0 foreign-key violations) and `pop-grpc-demo/pop.db` (v2), earlier backups (`player_backup`, `player_backup2`, including `objects.db.pre-v3` `14c67331...`), 13 rollback zips on the host (17 on the player in `/storage/sd/rollback/`).
Source snapshot of the installed implementation incl. uncommitted work: `frozen_demo_state/source/` (base commit `22c22e3`, `tracked_changes.patch`, `untracked_files.tar.gz`, SHA256SUMS). No file under `src/`, `libs/`, `dashboard/` or `dashboard-server/` is newer than the cross-built binary, so the snapshot's implementation is exactly what is installed; later edits are tests, scripts and docs only. Nothing is committed or pushed.

## Final walkthrough (live player, headless Chrome, `tests/browser/final_demo_walkthrough_cdp.py`): 34/34
Audience Measurement; Visual Memory gallery; camera full-frame Visual Search; camera automatic Object Search; Weston/VOP2 live preview (640x360); display full-frame Visual Search (10 cards, 0.97-0.99, Ad C, PoP links) and Search by Time; display automatic Object Search with a fresh Ad C session (UI backfill 40 objects: 20 bottle, 20 cup; bottle and cup searches from a stored frame, crops, similarity 0.94-0.96 / 0.92-0.99, confidence, offset, session, compare dialog, PoP session link; no "JPEG expired" tile); back to camera (camera objects only); Proof of Play correct (corroborated, 0.944), wrong (mismatch, 0.600), interrupted (incomplete, 0.878); presentation resumed; no console errors. Reference screenshots (23 files): `demo_reference_images/`.

## Independent recheck of full-frame Visual Search
* Camera (`/api/vm/search/frame`): 160 of 160 later searches succeeded (median 329 ms, p95 425 ms, top similarity 0.98-0.99, own frame excluded). **But 2 of about 280 calls returned HTTP 500** (a `DatabaseError` class response, one right after the install reboot and one during testing); I could not reproduce it again. Same family as the one lost Stage 1 write (`database_failures = 1`): a short SQLite busy on the camera store (journal mode DELETE, Argus writes continuously). Recovery: press Search again.
* Display (`/api/pop/search/display`, Ad C reference, with and without the ad filter): 5 hits, 87 ms, top similarity 0.988, all `ad-c`, offsets 5.8-6.0 s; in the UI 10 cards.

## Queue depth 8/8: periodic, not sustained
Existing diagnostics (`queue_depth`, `queue_capacity`, `skipped_stale`, `dropped_queue_full` per source; 150 samples per mode at 2 s, two matrices). The depth is a sawtooth that takes every value from 0/1 to 8 about equally often (each value 9-14 % of the samples): the live scanner hands over a batch of the newest frames (up to the capacity of 8) per scan and the worker drains it at the configured rate (camera 6/min, display 12/min). Time at 8/8: camera 13-14 %, display 9-12 %; the longest run at 8/8 was 5 samples (10 s) for the camera and 3 samples (6 s) for the display. **Dropped frames: 0 in every run**; `skipped_stale` (69 on the camera at the end of testing) counts frames deliberately sampled out because the worker is rate-limited, by design. Conclusion: transient and periodic, the queue is never saturated for long and never overflows at these rates. Limits not changed.

## Incremental RSS and Weston preview FPS
* `vm_api_server` RSS: 40.2 MB idle (baseline, models not loaded) -> 70.4 MB (camera only, +30.2), 76.1 MB (display only, +35.9), 74.8 MB (both, +34.6); mean +33 MB (peak/HWM +28 to +34 MB). The RKNN YOLOX context and the MobileNetV2 provider load on first use. PSS was not captured (not exposed).
* Weston preview FPS (active samples, bright scene): baseline mean 4.74 (median 5.0); camera only 4.49 (4.6); display only 4.67 (5.0); both **4.31 (4.4)**; the first matrix (dark scene) gave 4.57 / 4.73 / 4.62 / **4.38**. So the preview loses about 0.2-0.45 FPS (4-9 %) on average, most with both sources indexing (the share of samples under 4.5 FPS rose from 16 % to 58 %); the median stays 4.4-5.0 and D5 evidence capture was unaffected (40/40 sessions corroborated, 28-31 history frames per session). Noise between runs is about +-0.2 FPS, so only the "both" reduction is consistent in both matrices. Not optimised.

## Demo-readiness checklist
- [x] Package, config, autorun, databases and rollback preserved and hashed; source snapshot made; nothing pushed
- [x] Camera: Visual Memory, Visual Search, automatic Object Search
- [x] Weston/VOP2 live preview; display history about 1 FPS (28-31 frames per 30 s session)
- [x] Display Visual Search, Search by Time, automatic Object Search with Ad C, crops, PoP session links
- [x] PoP correct / wrong / interrupted verdicts; presentation resumes
- [x] No worker errors; INT8 bottle 15/15 and cup 15/15
- [x] Runbook with exact clicks, reference images, expected results and recovery steps
- [ ] Lights on in the room before the demo (camera FPS)

## Remaining issues (none blocks the demo)
1. **`VM: errors` red pill** (Stage 1 `database_failures = 1`): cosmetic but visible; it was 0 after the last reboot and became 1 again during testing. Cause: one lost camera-store write (SQLite busy), in the unchanged Argus binary. Left visible as instructed.
2. **Intermittent camera full-frame search 500** (about 0.7 %): retry works.
3. **Object-search panel sometimes missing on first load** (UI race, 3 of about 12 loads in headless Chrome): F5.
4. Cosmetics: "Display search is loading..." stays on the display page; the totals line lags the backfill by a few seconds; the picker shows only the newest 48 frames (choose a session).
5. Camera FPS drops to about 19 in dim light (default `exposure_auto_priority`, not changed).
6. Weston preview about 0.2-0.45 FPS lower with both sources indexing.
7. Ad C stays in the isolated demo registry (no delete API); the "expired JPEG" stage was never forced on the player (no evidence deleted); PSS not measured.
8. A stray untracked folder `argus-audience-measurement-extension/--root/` (test debris) is still in the repo; delete it when convenient (the path guard blocked my `rm -r`).
Blockers for the stakeholder demo: none identified.
