# Visual Memory: operating notes and troubleshooting

How to tell whether Visual Memory is really storing the latest images, why it might not be, how to turn on Stage 2
(embeddings), and how to deploy a new build. Contract for the status fields: `VISUAL_MEMORY_DASHBOARD_API.md`.

## 1. "Is it storing the latest images?"

Look at the **Visual Memory** tab, tile **Latest JPEG committed**, and the banner above the tiles:

| Dashboard | Meaning |
|---|---|
| `ingestion: ok`, age under ~10 s | A new JPEG is committed about every `sample_interval_ms`. |
| Red banner "Frame ingestion stalled" | Nothing has been stored for longer than max(3 x interval, 30 s). The message says why (expired-data cleanup, full queue). The health pill turns red. |
| Blue banner "Clearing expired frames from an earlier run" | The writer is deleting old data in the background. **New frames are still stored** (since the VM-21 fix). |
| Queue-full drops > 0 | The writer could not keep up when a sample arrived. Zero is expected in normal running. |
| Invalid frames > 0 | Samples rejected before queueing (bad dimensions or format). Not a storage problem. |

The gallery follows the newest page by itself (no reload needed): new frames are merged in place every status poll
(5 s). If you are on page 2, "Oldest first" or have an end-time filter, the view is left alone and a "Newer frames have
been stored" notice offers **Show newest**.

Capture FPS says nothing about storage. Argus ran at 29 FPS the whole time the store was not being written.

## 2. What went wrong on 2026-11-05 (VM-21)

Symptoms: status said 3,629 frames and "newest 13:27 UTC", the gallery showed frames from two days earlier, 128 of 175
samples were dropped, Stage 2 showed nothing.

1. **Stale store + blocking cleanup.** The store at `/storage/sd/visual-memory` held ~14,000 frames older than
   `retention_hours` (48 h) from an earlier test. At start-up the writer thread ran retention to completion
   (one file delete plus one SQLite commit per frame, about 20/s on the SD card, roughly 11 minutes) **before** it began
   draining its 4-frame queue. Argus kept sampling every 5 s, the queue stayed full, and every sample was counted as
   `dropped_queue_full`. Nothing new was stored until the pass ended (about 13:23:50 UTC). After that, drops stayed at 128
   and a JPEG was committed every 5 s (capture-to-commit p50 55 ms, p95 65 ms).
2. **Stale gallery.** The gallery fetched its page once when the tab opened. The status tiles kept polling, so the page
   showed fresh counts above two-day-old thumbnails until you pressed Refresh. The API itself returned the correct newest
   frames: a direct SQLite query of a copy of the database, `/api/vm/frames` and the status all agreed.
3. **No embeddings.** The active config only had `"enabled": true`. Stage 2 needs `vector_search_enabled: true` **and**
   an `embedding` block with `enabled: true` (see section 3). Without them `embedding_configured` is false and search is
   off. This was configuration, not a fault.

Fixes (all in this repository, see the commit log): retention runs in short budgeted slices between frames; the status
gains `ingest`, `storage_root`, last commit and latest-embedding times; the gallery follows the newest frames; drops are
split into queue-full and invalid; Stage-2-disabled and stall warnings are shown; backfill is capped.

Writer and API use the same storage root: both read `visual_memory.storage_root` from the active config
(`/storage/sd/configs/argus-config.json`), the database is `<storage_root>/visual_memory.db`, JPEGs are under
`<storage_root>/frames/YYYY/MM/DD/HH/<source>/` (hourly partitioning; older days keep the previous layout).

## 3. Turning on Stage 2 (embeddings)

Edit the **active** config and restart Argus (or reboot). Everything below `visual_memory`:

```json
"vector_search_enabled": true,
"embedding": {
  "enabled": true,
  "backend": "rknn",
  "model_path": "model/visual_memory/mobilenet_embedder.rknn",
  "max_pending": 4,
  "retention_days": 7,
  "backfill_on_startup": false,
  "npu_core": -1,
  "prefer_sqlite_vec": true
}
```

Check: `/api/vm/status` shows `embedding_configured: true`, `runtime.stage2.status: "active"`, `model` filled in, and
`store.latest_embedding_utc_ms` advancing (a new embedding about every 5 s). The model file must exist in the installed
extension (`/var/volatile/bsext/ext_npu_argus/model/visual_memory/mobilenet_embedder.rknn`); a missing or unloadable model
is reported on the dashboard ("Embeddings are configured but not running") and in the Argus log.

### Bounded backfill of old frames

Frames stored before embeddings were enabled stay unembedded. To make some of them searchable without a large one-off
job, set `backfill_on_startup: true`. It is bounded on purpose:

* `backfill_max_frames` (default **500**): at most that many frames are attempted per Argus run, newest first.
* `backfill_max_age_hours` (default **24**): only frames captured within that window.
* It runs in the embedder's idle time in batches of 8 with a 250 ms pause, so live embedding of new frames comes first.
* `0` removes a limit and is an explicit opt-in. Do not run it unbounded against the existing store.

Progress is `runtime.stage2.backfill_attempted / backfill_cap` in the status snapshot; `backfill_done` turns true at the cap.
Restarting Argus allows another batch of the same size.

## 4. Deployment procedure for a new build (needs approval; nothing here is run automatically)

Candidate build for the VM-21 fix: `argus-ext-1793887714.zip` (RK3588 only, sha256
`d995e1aa4a95be6d2cb80fcd2a33cbc076e569aae0c99da916bd507b69dc2574`). Build it with
`EXTRA_CMAKE_FLAGS="-DENABLE_VISUAL_MEMORY=ON -DENABLE_VISUAL_EMBEDDINGS=ON" make build SOCS=rk3588` then
`./package --ext-only --soc RK3588` (plain `make package` also packages the RK3576/RK3568 models, and without the flags it
silently builds **without** Visual Memory; check that the zip contains `RK3588/vm_api_server`). The packaging step deletes
older local zips, so copy the current one aside first.

Do not change the active config, install a package or reboot without sign-off. The current package and the rollback zips
stay on the card.

1. **Record the baseline**: `sha256sum` of `/storage/sd/configs/argus-config.json` and `/storage/sd/autorun.brs`;
   `extensions` in the BrightSign shell; the installed zip name. Keep `argus-ext-1793873298.zip` (current) and the older
   rollback zips.
2. **Copy** the new zip: `scp argus-ext-<new>.zip brightsign@<player>:/storage/sd/` (additive; deletes nothing).
3. **Install** from the root shell (the presentation stops while the shell is open): `bsext_init stop`; stop mosquitto;
   `cd /usr/local && unzip -o /storage/sd/argus-ext-<new>.zip && bash ./ext_npu_argus_install-lvm.sh`; `sync`. Confirm the
   installer printed "Renamed ..." and its SHA-256 check passed.
4. **Reboot for real**: `PUT /api/v1/control/reboot` on the DWS (or `reboot` in the root shell). `exit` only restarts the
   player software and does not remount a replaced extension volume.
5. **Verify** (read-only): `GET :20300/api/vm/status` gives `ingest.state: ok`; `runtime.stage1.encoded` rises every
   5 s, `dropped_queue_full` stays 0, `queue_depth` 0-1; Argus FPS about 29; the gallery's top frame is current; with
   Stage 2 on, `latest_embedding_utc_ms` advances.
6. **Roll back** if anything is off: repeat steps 3-4 with the previous zip.

If the store still holds a large expired backlog, the fixed writer clears it in the background without blocking new
frames. To start from a clean store instead, point `visual_memory.storage_root` at a new directory; the old store is
left untouched (deleting it is a separate, explicit decision).
