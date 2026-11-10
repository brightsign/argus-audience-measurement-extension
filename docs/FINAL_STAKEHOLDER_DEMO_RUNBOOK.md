# Final stakeholder demo runbook (Argus + Visual Memory + Proof of Play + camera/display Object Search)

Frozen state (2026-11-09): package `argus-ext-1794251841.zip` (sha256 `8bed0a93841565f0abba61e293ff08d3cbc25783e0d5923035d5607450118513`) on the development XT5 `192.168.0.165`, dashboard `http://192.168.0.165:20300`.
Active configuration = original + one block `objects.auto.display {enabled:true, live_scan:false, frames_per_minute:12, queue_depth:8}` (sha `329c58ec...`, copy and hashes in `hw_validation_20261109/frozen_demo_state/`). Autorun unchanged (`394d7be1...`).
Known-good reference screenshots: `/home/sree/bs/sqlite_argus/hw_validation_20261109/demo_reference_images/` (`NN_name.png`, produced by `tests/browser/final_demo_walkthrough_cdp.py`, 34/34 on this build). They are kept outside Git (the camera pages show live audience crops).
Do NOT during the demo: change the configuration, press anything under Proof of Play other than the three Verify buttons and Schedule play, reinstall, reboot, or restart services. Total time about 15 minutes.

## 0. Before the audience arrives (3 min)
1. Light: switch the room lights on. The camera lowers its frame rate in dim light (default exposure priority; dark = 19 FPS, bright = 29 FPS). Open **Audience Measurement**: top-left should say `30 fps`/`29 fps`.
2. Open `http://192.168.0.165:20300`. Header pills: `live` green. **`VM: errors` may be red**: it shows the cumulative Stage 1 counter `database_failures = 1` (one lost camera-store write since boot, intentionally not hidden). Say so if asked; nothing is broken (Visual Memory keeps storing).
3. Proof of Play: the media table lists `ad-a`, `ad-b`, `ad-c`; the Demonstration box shows Ad A / Ad B and enabled buttons; the preview panel shows the player screen.
4. Run **one fresh Ad C play** now so its frames are the newest: Proof of Play -> *Schedule a play* -> Expected advertisement `Product Test (bottle and cup) (ad-c)` -> **Schedule play**. Wait 35 s until the session shows `Playback corroborated` (reference: `19_pop_correct.png` is the same page for Ad A).

## 1. Audience Measurement (1 min)
Click **Audience Measurement**. Expect: LIVE, 29-30 fps, PRESENT NOW / GAZING NOW, attention rate, NPU load about 80 %. (`01_audience_measurement.png`)

## 2. Camera Visual Memory and Visual Search (2 min)
1. **Visual Memory**: a gallery of stored camera frames with thumbnails (`02_visual_memory.png`).
2. **Visual Search** -> source **Audience Camera** -> **Full frame search** -> **Pick a stored frame...** -> click any thumbnail -> **Search**. Expect 5 similar frames with similarity 0.98-0.99 in about 0.3-0.5 s (`03_camera_visual_search.png`).
3. **Object search**: the *Automatic object indexing* panel (YOLOX-S, live scan on, 6 frames/min) and indexed camera objects (cup/bottle/...) (`04_camera_object_search.png`).
   Recovery: if the panel is missing, press **F5** (a known intermittent page-load race; a reload fixes it).

## 3. Weston/VOP2 live preview (1 min)
**Proof of Play** page, panel *Display output preview*: a live 640x360 picture of what the player shows, about 5 FPS (`05_weston_live_preview.png`).

## 4. Display Visual Search and Search by Time (3 min)
1. **Visual Search** -> **BrightSign Display (Weston/VOP2)** (the text "Display search is loading..." at the top stays; ignore it).
2. *Search by time*: set the date/time shown for any Ad C moment (e.g. use the "Use newest stored frame time" button), **Search by time**. Expect the nearest stored frame with ad `ad-c`, offset, uncertainty +-252 ms, the JPEG and a link to the session (`07_search_by_time.png`).
3. *Reference*: drop `display_search_reference_5000ms.jpg` (in the reference-image folder) on the reference box, Advertisement `Product Test (bottle and cup)`, **Search display evidence**. Expect 10 cards, similarity 0.97-0.99, "Playback corroborated", "into the play" 5.8-6.1 s (`06_display_visual_search.png`). Click **Open playback session** on a card to show the PoP session.

## 5. Display automatic Object Search with Ad C (4 min)
1. **Visual Search** -> **Object search** -> **BrightSign Display (Weston/VOP2)**.
2. Click **Index the 50 most recent retained frames** (the panel's button). About 20-25 s; the state line ends with `Backfill done`. The totals line refreshes a few seconds later: about 40 new objects (20 bottle, 20 cup per Ad C session); classes `bottle ... | cup ...` (`08_display_object_search_totals.png`).
3. **Select object in a stored display frame...** -> choose a frame of the newest session with an offset of 4-8 s (they are listed newest first; none shows "JPEG expired") -> in the box fields type the bottle box: use the values of `09_display_bottle_selected_box.png` (about X 163-170, Y 215-218, W 29-31, H 97-101) -> **Find Similar Objects**. Expect only display cards, similarity 0.94-0.96, "bottle", 83 % detector confidence, ad `ad-c`, offset and `session s-...`, crop thumbnails (`10_display_bottle_results.png`).
4. Click the first card: the comparison dialog shows the source frame with the box, similarity, timestamp and **Open Proof-of-Play session** (`11_...compare.png`); click it: the session opens (`12_display_bottle_pop_session.png`).
5. Repeat with the cup (X about 296-300, Y 225, W 148, H 90): similarity 0.92-0.99, "cup", 94 % confidence (`13`-`16`).
6. Switch **Source** back to **Audience Camera**: camera objects only (`17_back_to_camera.png`). Stress the point: the two sources never mix unless "All sources" is chosen explicitly.
   Note: the picker shows only the newest 48 stored frames unless a session is chosen in its drop-down; after other plays (Ad A/B), run another Ad C play first (step 0.4).

## 6. Proof of Play: correct, wrong, interrupted (4 min)
On **Proof of Play** -> *Demonstration* (Ad A expected, Ad B wrong ad, capture Resilience gRPC):
| click | wait | expected verdict |
|---|---|---|
| **Verify Correct Ad** | 35 s | `Playback corroborated`, 3/3 checkpoints, mean similarity 0.94-0.99 (`19_pop_correct.png`) |
| **Verify Wrong Ad** | 35 s | `Mismatch`, mean similarity about 0.60 (`20_pop_wrong.png`) |
| **Verify Interrupted Ad** (confirm the dialog) | 20 s | `Incomplete`, played about 10 s, 1 of 3 checkpoints (`21_pop_interrupted.png`) |
The presentation resumes by itself after each play; buttons unlock when a verdict is shown. Interrupted stops the screen video for about 10 s: warn the audience.

## Recovery steps
| symptom | do |
|---|---|
| a search shows "Search failed" / HTTP 500 (camera full-frame, seen 2 times in about 280 searches) | press **Search** again; it is a short SQLite busy on the camera store |
| automatic indexing panel missing | **F5** |
| display picker shows no Ad C frame | run a fresh Ad C play (0.4) or choose its session in the picker's session drop-down |
| indexing button seems idle | wait for `Backfill done`; then **F5** if the totals do not move |
| a play does not start in 10 s | wait; if the Demonstration buttons stay locked more than 2 minutes, reload the page; do not reboot |
| camera FPS about 19 | the room is dark: turn the lights on (no setting change is needed) |
| `VM: errors` red | expected (see 0.2) |
| anything else | stop; nothing here requires a restart. Rollback exists but is NOT a demo-time action: see VISUAL_MEMORY_DISPLAY_OBJECTS_DEPLOYMENT_PLAN.md |

## Things the numbers mean (for questions)
* Detection on the real INT8 NPU model for Ad C: bottle 15/15 and cup 15/15 frames, no bottle/cup in the unrelated scene; YOLOX 33-35 ms per frame, MobileNetV2 15-21 ms per crop. Object search covers only indexed objects, never every stored frame.
* Indexing adds about 33 MB RSS (40 -> 70-76 MB for `vm_api_server`), no change in Argus FPS (29) or D5 results, a 0.2-0.4 FPS lower Weston preview average when both sources index (see the measurement notes in the readiness document).
