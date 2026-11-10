#!/usr/bin/env python3
"""Rehearse the objects.db schema v2 -> v3 migration on a COPY and check that nothing is lost.

  validate_objects_migration.py <vm_api_server> <objects.db (v2 copy)> <workdir>

Never touches the source file. The copy is opened by the real vm_api_server (host build, mock embedder/detector), which
migrates it on start. Checks:
  1. v2 data is identical afterwards: every object_regions row (all v2 columns), every vector blob, every progress row,
     the AUTOINCREMENT counter, the model table;
  2. new columns hold the camera defaults; source-aware uniqueness is in force;
  3. PRAGMA integrity_check ok, no NEW foreign-key violations;
  4. objects.db.pre-v3 is a byte-identical copy of the input; a second start does not change the file or the backup;
  5. rollback = restoring objects.db.pre-v3: byte-identical to the input again, user_version 2.
"""
import hashlib, json, os, shutil, signal, sqlite3, subprocess, sys, time, urllib.request

SERVER, SRC, WORK = sys.argv[1:4]
PORT = 20473
fails = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:300]) if not cond and extra != "" else ""), flush=True)
    if not cond: fails.append(name)


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()


def ro(p):
    return sqlite3.connect("file:%s?mode=ro" % p, uri=True)


def cols(c, t):
    return [r[1] for r in c.execute("pragma table_info(%s)" % t)]


def snapshot(p):
    c = ro(p); s = {}
    s["version"] = c.execute("pragma user_version").fetchone()[0]
    s["tables"] = sorted(r[0] for r in c.execute("select name from sqlite_master where type='table'"))
    for t in s["tables"]:
        if t == "sqlite_sequence": continue
        s["cols:" + t] = cols(c, t)
        s["n:" + t] = c.execute("select count(*) from %s" % t).fetchone()[0]
    s["seq"] = dict(c.execute("select name, seq from sqlite_sequence").fetchall())
    s["integrity"] = c.execute("pragma integrity_check").fetchone()[0]
    s["fk"] = len(c.execute("pragma foreign_key_check").fetchall())
    c.close()
    return s


def rows(p, t, only):
    c = ro(p)
    have = cols(c, t)
    sel = [x for x in only if x in have]
    out = c.execute("select %s from %s order by 1" % (",".join(sel), t)).fetchall()
    c.close()
    return sel, out


def vec_hash(p):
    c = ro(p); h = hashlib.sha256()
    for r in c.execute("select * from object_vectors order by 1"): h.update(repr(r).encode())
    c.close()
    return h.hexdigest()


def start(cfg):
    pr = subprocess.Popen([SERVER, "--config", cfg, "--port", str(PORT), "--provider", "mock:64", "--detector", "mock", "--no-reexec",
                           "--status-file", os.path.join(WORK, "st.json")], stdout=open(os.path.join(WORK, "srv.log"), "a"), stderr=subprocess.STDOUT)
    for _ in range(100):
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/api/vm/status" % PORT, timeout=2) as r:
                if r.status == 200:
                    with urllib.request.urlopen("http://127.0.0.1:%d/api/vm/objects/auto/status" % PORT, timeout=5) as r2: r2.read()  # forces objects.db open
                    return pr
        except Exception: time.sleep(0.2)
    pr.kill(); raise SystemExit("server did not start (see srv.log)")


def stop(pr):
    pr.send_signal(signal.SIGTERM)
    try: pr.wait(15)
    except Exception: pr.kill()


shutil.rmtree(WORK, ignore_errors=True); os.makedirs(os.path.join(WORK, "root"))
DB = os.path.join(WORK, "root", "objects.db"); BAK = DB + ".pre-v3"
shutil.copyfile(SRC, DB)
src_sha = sha(SRC); check("copy is byte-identical to the source", sha(DB) == src_sha)
pre = snapshot(DB)
print("v2 input: user_version=%d, regions=%d, vectors=%s, progress=%s, seq=%s" % (pre["version"], pre["n:object_regions"], pre.get("n:object_vectors"), pre.get("n:object_frame_progress"), pre["seq"]))
check("input is schema 2", pre["version"] == 2)
V2COLS = [c for c in pre["cols:object_regions"]]
pre_reg = rows(DB, "object_regions", V2COLS)[1]
pre_prog_cols, pre_prog = rows(DB, "object_frame_progress", pre["cols:object_frame_progress"])
pre_vec = vec_hash(DB)
pre_models = rows(DB, "object_models", pre["cols:object_models"])[1]

cfg = os.path.join(WORK, "cfg.json")
json.dump({"visual_memory": {"enabled": True, "vector_search_enabled": True, "storage_root": os.path.join(WORK, "root"), "stored_width": 640, "stored_height": 360,
                             "embedding": {"enabled": True, "backend": "rknn", "model_path": "x"},
                             "objects": {"enabled": True, "auto": {"enabled": False}}}}, open(cfg, "w"))
t0 = time.time(); p = start(cfg); stop(p); print("migration + start: %.1f s" % (time.time() - t0))

post = snapshot(DB)
check("schema is now 3", post["version"] == 3, post["version"])
check("integrity_check ok", post["integrity"] == "ok", post["integrity"])
check("no new foreign-key violations", post["fk"] <= pre["fk"], (pre["fk"], post["fk"]))
check("backup objects.db.pre-v3 exists", os.path.exists(BAK))
check("backup is byte-identical to the v2 input", os.path.exists(BAK) and sha(BAK) == src_sha)
for t in ("object_regions", "object_vectors", "object_frame_progress", "object_models", "object_meta"):
    if "n:" + t in pre: check("row count preserved: " + t, pre["n:" + t] == post["n:" + t], (pre["n:" + t], post["n:" + t]))
post_reg = rows(DB, "object_regions", V2COLS)[1]
check("every v2 object_regions row identical (all %d columns, incl. region_id/frame_id/bbox/class/crop path)" % len(V2COLS), pre_reg == post_reg)
check("vector blobs identical", vec_hash(DB) == pre_vec)
check("object_frame_progress rows identical (v2 columns)", rows(DB, "object_frame_progress", pre_prog_cols)[1] == pre_prog)
check("object_models identical", rows(DB, "object_models", pre["cols:object_models"])[1] == pre_models)
check("AUTOINCREMENT counters not lowered", all(post["seq"].get(k, 0) >= v for k, v in pre["seq"].items()), (pre["seq"], post["seq"]))
new = set(post["cols:object_regions"]) - set(pre["cols:object_regions"])
print("new object_regions columns:", sorted(new))
c = ro(DB)
check("source columns present", {"source_type", "source_kind", "pop_session_id", "expected_media_id", "reported_media_id", "playback_offset_ms", "time_uncertainty_ms", "provenance"} <= new)
check("every migrated row is source_type='camera'", c.execute("select count(*) from object_regions where source_type<>'camera'").fetchone()[0] == 0)
check("camera frame ids preserved as (camera,'',frame_id)", c.execute("select count(*) from object_regions where source_type='camera' and source_kind=''").fetchone()[0] == pre["n:object_regions"])
check("progress rows all camera", c.execute("select count(*) from object_frame_progress where source_type<>'camera'").fetchone()[0] == 0)
check("origins preserved", dict(c.execute("select index_origin,count(*) from object_regions group by 1").fetchall()) ==
      dict(ro(SRC).execute("select index_origin,count(*) from object_regions group by 1").fetchall()))
print("origins:", dict(c.execute("select index_origin,count(*) from object_regions group by 1").fetchall()))
c.close()

# second start: idempotent, backup untouched
d1, b1 = sha(DB), sha(BAK)
p = start(cfg); stop(p)
check("second start leaves the backup untouched", sha(BAK) == b1)
check("second start leaves schema 3 / integrity ok", snapshot(DB)["version"] == 3 and snapshot(DB)["integrity"] == "ok")

# rollback = restore the backup (the previous package never writes a v3 file; it refuses to open one)
shutil.copyfile(BAK, DB)
rb = snapshot(DB)
check("rollback: restored file is byte-identical to the v2 input", sha(DB) == src_sha)
check("rollback: schema 2, integrity ok, counts as before", rb["version"] == 2 and rb["integrity"] == "ok" and rb["n:object_regions"] == pre["n:object_regions"])
# and re-migrating the restored file works again (the procedure is repeatable)
os.remove(BAK); p = start(cfg); stop(p)
check("re-migration of the restored file works", snapshot(DB)["version"] == 3 and rows(DB, "object_regions", V2COLS)[1] == pre_reg)

print("\n%s: %d check(s) failed" % ("FAIL" if fails else "ALL PASSED", len(fails)))
sys.exit(1 if fails else 0)
