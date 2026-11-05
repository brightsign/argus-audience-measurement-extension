' ============================================================================
' Argus D5 Proof-of-Play test autorun (DEVELOPMENT XT5 ONLY, not a production player script).
'
' Two modes, chosen at boot:
'   * SD:/pop/enabled absent  -> exactly the original presentation behaviour (loop meet-brightsign.mp4).
'   * SD:/pop/enabled present -> job mode: polls SD:/pop/jobs/*.json, plays the requested media, reports
'     playback events to SD:/pop/events.jsonl and captures the REAL presentation output with
'     roVideoMode.Screenshot() into SD:/pop/shots/.
'
' This firmware has NO usable wall clock in BrightScript (roDateTime is epoch 0, roSystemTime fails), so every event
' carries mono_ms (roTimespan since this script started) and the consumer (vm_api_server) stamps UTC when it reads
' the line. A "boot" event is written at start so the consumer can re-anchor the clock.
'
' Job file (JSON), all keys optional except media_file:
'   job_id, session_id, media_id, expected_media_id   identifiers copied into every event
'   media_file          file name on the SD card, or an rtsp:// URL
'   max_play_ms         stop playback this long after it started (slot length), default 30000
'   interrupt_at_ms     stop EARLY after this long (simulates an interrupted ad); default none
'   loop                loop the media (needed when the media is shorter than max_play_ms), default false
'   capture_offsets_ms  e.g. [3000,15000,27000] (offset from the playback-start event)
'   capture_dir         where screenshots go (default SD:/pop/shots); an unwritable path makes the real
'                       Screenshot() call fail, which is how the "screenshot unavailable" case is exercised
'   capture_width/height  default 640x400
'
' Rollback: copy SD:/autorun.brs.pre-d5 over SD:/autorun.brs (or delete SD:/pop/enabled), then reboot.
' ============================================================================
Sub Main()
    ctx = {}
    ctx.tsp = CreateObject("roTimespan")
    ctx.tsp.Mark()
    CreateDirectory("SD:/pop")
    CreateDirectory("SD:/pop/jobs")
    CreateDirectory("SD:/pop/done")
    CreateDirectory("SD:/pop/shots")
    ctx.port = CreateObject("roMessagePort")
    ctx.vid = CreateObject("roVideoPlayer")
    ctx.vid.SetPort(ctx.port)
    ctx.vm = CreateObject("roVideoMode")
    ctx.log = CreateObject("roAppendFile", "SD:/pop/events.jsonl")
    ctx.seq = 0
    enabled = ReadAsciiFile("SD:/pop/enabled")
    if Len(enabled) = 0 then
        DefaultLoop(ctx)
        return
    end if
    Ev(ctx, { ev: "boot", mode: "jobs", output: ctx.vm.GetMode() })
    while true
        job = NextJob(ctx)
        if job <> invalid then
            RunJob(ctx, job)
        else
            msg = wait(300, ctx.port)
        end if
    end while
End Sub

' ---- original behaviour ----------------------------------------------------
Sub DefaultLoop(ctx as Object)
    r = CreateObject("roRectangle", 0, 0, 1920, 1080)
    ctx.vid.SetRectangle(r)
    ctx.vid.SetLoopMode(true)
    ok = ctx.vid.PlayFile({ Filename: "meet-brightsign.mp4" })
    print "PlayFile returned: "; ok
    while true
        msg = wait(0, ctx.port)
        if type(msg) = "roVideoEvent" then
            print "roVideoEvent: "; msg.GetInt()
        end if
    end while
End Sub

' ---- job discovery ---------------------------------------------------------
Function NextJob(ctx as Object) as Dynamic
    names = ListDir("SD:/pop/jobs")
    if names = invalid then return invalid
    best = ""
    for each n in names
        if Len(n) > 5 and Right(n, 5) = ".json" then
            if best = "" or n < best then best = n
        end if
    end for
    if best = "" then return invalid
    txt = ReadAsciiFile("SD:/pop/jobs/" + best)
    MoveFile("SD:/pop/jobs/" + best, "SD:/pop/done/" + best)
    job = ParseJson(txt)
    if job = invalid or not job.DoesExist("media_file") then
        Ev(ctx, { ev: "job_invalid", file: best })
        return invalid
    end if
    job.job_file = best
    return job
End Function

Function JV(job as Object, key as String, dflt as Dynamic) as Dynamic
    if job.DoesExist(key) then return job[key]
    return dflt
End Function

' ---- one playback session --------------------------------------------------
Sub RunJob(ctx as Object, job as Object)
    sid = JV(job, "session_id", "s" + Stri(ctx.tsp.TotalMilliseconds()).Trim())
    base = { session: sid, job_id: JV(job, "job_id", ""), media_id: JV(job, "media_id", ""), expected_media_id: JV(job, "expected_media_id", "") }
    maxPlay = JV(job, "max_play_ms", 30000)
    interruptAt = JV(job, "interrupt_at_ms", -1)
    offsets = JV(job, "capture_offsets_ms", [])
    capDir = JV(job, "capture_dir", "SD:/pop/shots")
    capW = JV(job, "capture_width", 640)
    capH = JV(job, "capture_height", 400)
    done = []
    for i = 0 to offsets.Count() - 1
        done.Push(false)
    end for
    ctx.vid.SetLoopMode(JV(job, "loop", false))
    e = CopyAA(base)
    e.ev = "session_scheduled_run"
    e.media_file = job.media_file
    e.max_play_ms = maxPlay
    e.interrupt_at_ms = interruptAt
    e.capture_count = offsets.Count()
    Ev(ctx, e)
    reqMs = ctx.tsp.TotalMilliseconds()
    ' Streams (rtsp/http/udp) must be passed with the "Url" key on this firmware: {Filename:"rtsp://..."} and a plain
    ' string return false (verified on the XT5, BrightSignOS 9.1.139), while {Url:"rtsp://..."} plays.
    mf = job.media_file
    if Instr(1, mf, "://") > 0 then
        ok = ctx.vid.PlayFile({ Url: mf })
    else
        ok = ctx.vid.PlayFile({ Filename: mf })
    end if
    e = CopyAA(base)
    e.ev = "play_requested"
    e.ok = ok
    Ev(ctx, e)
    if not ok then
        e = CopyAA(base)
        e.ev = "play_error"
        e.reason = "PlayFile returned false"
        Ev(ctx, e)
        EndSession(ctx, base, "play_failed", 0)
        return
    end if
    startMs = -1
    endReason = ""
    while true
        msg = wait(100, ctx.port)
        now = ctx.tsp.TotalMilliseconds()
        if type(msg) = "roVideoEvent" then
            code = msg.GetInt()
            e = CopyAA(base)
            e.vcode = code
            e.vdata = msg.GetData()
            if code = 3 and startMs < 0 then
                startMs = now
                e.ev = "play_started"
                e.latency_ms = now - reqMs
            else if code = 8 then
                e.ev = "play_ended"
                endReason = "ended"
            else
                e.ev = "video_event"
            end if
            Ev(ctx, e)
            if endReason = "ended" then exit while
        end if
        if startMs < 0 and now - reqMs > 15000 then
            e = CopyAA(base)
            e.ev = "play_error"
            e.reason = "no playback-start event within 15 s"
            Ev(ctx, e)
            endReason = "start_timeout"
            exit while
        end if
        if startMs >= 0 then
            el = now - startMs
            for i = 0 to offsets.Count() - 1
                if not done[i] and el >= offsets[i] then
                    done[i] = true
                    Capture(ctx, base, i + 1, offsets[i], capDir, capW, capH, startMs)
                end if
            end for
            if interruptAt >= 0 and el >= interruptAt then
                ctx.vid.Stop()
                e = CopyAA(base)
                e.ev = "play_stopped"
                e.reason = "interrupted"
                Ev(ctx, e)
                endReason = "interrupted"
                exit while
            end if
            if el >= maxPlay then
                ctx.vid.Stop()
                e = CopyAA(base)
                e.ev = "play_stopped"
                e.reason = "slot_end"
                Ev(ctx, e)
                endReason = "slot_end"
                exit while
            end if
        end if
    end while
    actual = 0
    if startMs >= 0 then actual = ctx.tsp.TotalMilliseconds() - startMs
    EndSession(ctx, base, endReason, actual)
End Sub

Sub EndSession(ctx as Object, base as Object, reason as String, actualMs as Integer)
    e = CopyAA(base)
    e.ev = "session_end"
    e.reason = reason
    e.actual_play_ms = actualMs
    Ev(ctx, e)
End Sub

Sub Capture(ctx as Object, base as Object, idx as Integer, targetOffset as Integer, capDir as String, w as Integer, h as Integer, startMs as Integer)
    fn = capDir + "/" + base.session + "_" + Stri(idx).Trim() + ".jpg"
    t0 = ctx.tsp.TotalMilliseconds()
    r = ctx.vm.Screenshot({ filename: fn, filetype: "jpeg", quality: 85, width: w, height: h })
    t1 = ctx.tsp.TotalMilliseconds()
    e = CopyAA(base)
    if r = true then
        e.ev = "capture_ok"
    else
        e.ev = "capture_failed"
        e.reason = ctx.vm.GetFailureReason()
    end if
    e.idx = idx
    e.file = fn
    e.target_offset_ms = targetOffset
    e.actual_offset_ms = t0 - startMs
    e.took_ms = t1 - t0
    e.width = w
    e.height = h
    ps = ctx.vid.GetPlaybackStatus()
    if type(ps) = "roAssociativeArray" then
        e.pstatus = FormatJson(ps)
    end if
    Ev(ctx, e)
End Sub

Function CopyAA(a as Object) as Object
    b = {}
    for each k in a
        b[k] = a[k]
    end for
    return b
End Function

Sub Ev(ctx as Object, e as Object)
    ctx.seq = ctx.seq + 1
    e.seq = ctx.seq
    e.mono_ms = ctx.tsp.TotalMilliseconds()
    s = FormatJson(e)
    print s
    ctx.log.SendLine(s)
    ctx.log.Flush()
End Sub
