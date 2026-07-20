package com.mimic.client

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.FileProvider
import com.mimic.client.capability.CapabilityManager
import com.mimic.client.capture.CaptureController
import com.mimic.client.input.InputController
import com.mimic.client.input.MimicAccessibilityService
import com.mimic.client.input.ShowTouchesHelper
import com.mimic.client.peer.PeerSession
import com.mimic.client.target.AppEnumerator
import com.mimic.client.target.AppLauncher
import com.mimic.client.target.TargetDescriptor
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

/**
 * Native side of hostCall for shared/web — logs + APK update + gate SSOT;
 * capture / peer / privilege backends are filled in progressively (铁律 5: no fake ok).
 */
class AndroidHost(
    private val context: Context,
    private val pushToJs: (JSONObject) -> Unit,
) {
    private val io = Executors.newCachedThreadPool()
    private val main = Handler(Looper.getMainLooper())
    private val tag = "MimicHost"
    private val prefs = context.getSharedPreferences("mimic_host", Context.MODE_PRIVATE)
    private val logDir = File(context.filesDir, "log").also { it.mkdirs() }
    private val logFile = File(logDir, "live.log")
    private val ring = ArrayDeque<String>(2000)

    // Gate SSOT — UI may open gates, but stream/control still fail until backends exist.
    @Volatile private var allowStream = false
    @Volatile private var acceptControl = false
    @Volatile private var activeTargetId: String = "display:0"
    /** Last outbound H.264 packed frame for on-device controlled preview. */
    @Volatile private var lastLocalPreview: ByteArray? = null
    @Volatile private var lastPreviewPushMs = 0L
    /** Local preview notify interval — base64 decode is expensive on WebView. */
    private val previewPushIntervalMs = 200L
    /** After MediaProjection consent, start capture if stream gate / set_target requested it. */
    @Volatile private var pendingStartAfterConsent = false
    private val caps = CapabilityManager(context)
    private val capture = CaptureController(context, caps)
    private val input = InputController(context, caps)
    private val peer = PeerSession(context, pushToJs)
    /** Set by MainActivity to launch the system screen-capture consent dialog. */
    var requestProjection: (() -> Unit)? = null

    init {
        rotateLogIfNeeded()
        caps.onStatusChanged = {
            main.post {
                pushToJs(
                    JSONObject()
                        .put("type", "capability_status")
                        .put("status", caps.statusJson()),
                )
            }
        }
        MimicAccessibilityService.ourPackage = context.packageName
        MimicAccessibilityService.relaunchConfined = { pkg, act ->
            appendLog("confine", "re-launch $pkg (left confined app)")
            AppLauncher.launch(context, pkg, act)
        }
        capture.onEncodedFrame = { packed ->
            peer.sendH264Packed(packed)
            lastLocalPreview = packed
            val now = System.currentTimeMillis()
            if (now - lastPreviewPushMs >= previewPushIntervalMs) {
                lastPreviewPushMs = now
                main.post {
                    pushToJs(JSONObject().put("type", "local_preview"))
                }
            }
        }
        capture.onCaptureEnded = {
            allowStream = false
            appendLog("cap", "capture ended (projection revoked or stop)")
            pushGates()
        }
        peer.onControlAction = { action ->
            if (acceptControl) input.inject(action, caps.active)
            else JSONObject().put("ok", false).put("error", "accept_control gate closed")
        }
        peer.onListTargets = { listTargets() }
        peer.onSetTarget = { json -> applyRemoteSetTarget(json) }
        peer.onRequestKeyframe = {
            appendLog("peer", "need_key → requestKeyframe")
            capture.requestKeyframe()
        }
        peer.onControlledMediaReady = {
            ensureDefaultControlledCapture()
        }
        peer.onSessionEnd = {
            allowStream = false
            acceptControl = false
            pendingStartAfterConsent = false
            MimicAccessibilityService.clearConfine()
            ShowTouchesHelper.restore(context)
            try { capture.stop() } catch (_: Exception) {}
            appendLog("peer", "session_end → gates closed")
            pushGates()
        }
        val prevHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { t, e ->
            try {
                appendLog("crash", "uncaught in ${t.name}: ${e.javaClass.simpleName}: ${e.message}")
            } catch (_: Exception) {
            }
            Log.e(tag, "uncaught", e)
            try {
                prevHandler?.uncaughtException(t, e)
            } catch (_: Exception) {
            }
        }
    }

    /** Activity resume — heal signaling if OEM froze WS while on Home. */
    fun onActivityResume() {
        try {
            peer.ensureOnline()
        } catch (e: Exception) {
            Log.w(tag, "ensureOnline", e)
        }
    }

    private fun pushGates() {
        val o = JSONObject()
            .put("type", "gates")
            .put("allow_stream", allowStream)
            .put("accept_control", acceptControl)
        main.post { pushToJs(o) }
    }

    private fun rotateLogIfNeeded() {
        try {
            if (logFile.exists() && logFile.length() > 0) {
                val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
                val archived = File(logDir, "session_$stamp.log")
                if (!logFile.renameTo(archived)) {
                    logFile.copyTo(archived, overwrite = true)
                    logFile.writeText("")
                }
            }
            // Keep last 20 session files
            val sessions = logDir.listFiles { f -> f.name.startsWith("session_") && f.name.endsWith(".log") }
                ?.sortedByDescending { it.lastModified() }
                ?: emptyList()
            sessions.drop(20).forEach { it.delete() }
        } catch (e: Exception) {
            Log.w(tag, "log rotate", e)
        }
    }

    /**
     * Controlled + LAN ready: default to Main Display without waiting for controller set_target.
     * Prompts MediaProjection consent if needed; local preview appears after encoder starts.
     */
    private fun ensureDefaultControlledCapture() {
        if (peer.role != "controlled") return
        if (capture.streaming) return
        // Do not override an active/pending app sandbox target.
        if (activeTargetId.startsWith("app:")) return
        val tid = if (activeTargetId.startsWith("display:")) activeTargetId else "display:0"
        activeTargetId = tid
        allowStream = true
        acceptControl = true
        main.post {
            pushToJs(
                JSONObject()
                    .put("type", "gates")
                    .put("allow_stream", true)
                    .put("accept_control", true)
                    .put("target_id", tid),
            )
        }
        appendLog("peer", "controlled default target=$tid")
        if (!capture.hasProjectionConsent()) {
            pendingStartAfterConsent = true
            appendLog("cap", "request MediaProjection for default Main Display")
            main.post { requestProjection?.invoke() }
            return
        }
        io.execute {
            val started = startCaptureForActiveTarget(tid)
            if (!started.optBoolean("ok", false)) {
                appendLog("cap", "default capture failed: ${started.optString("error")}")
            }
        }
    }

    /**
     * Controller picked a remote target — apply on controlled Android (parity with PC).
     * display:* → MediaProjection whole screen.
     * app:* → Shizuku VirtualDisplay sandbox only (铁律 5: no soft-confine fake).
     */
    private fun applyRemoteSetTarget(json: JSONObject): JSONObject {
        val id = json.optString("id", json.optString("target_id", ""))
        val tid = when {
            id.isNotBlank() -> id
            json.optLong("hwnd", -1L) == 0L -> "display:0"
            else -> activeTargetId
        }
        if (tid.isBlank()) {
            return JSONObject().put("ok", false).put("error", "missing target id")
        }
        val prevTarget = activeTargetId
        activeTargetId = tid
        appendLog("peer", "set_target id=$tid")

        allowStream = true
        acceptControl = true
        main.post {
            pushToJs(
                JSONObject()
                    .put("type", "gates")
                    .put("allow_stream", true)
                    .put("accept_control", true)
                    .put("target_id", tid),
            )
        }

        if (tid.startsWith("app:")) {
            MimicAccessibilityService.clearConfine()
            input.vdDisplayActive = true
            // Stop any MediaProjection stream first.
            try { capture.stop() } catch (_: Exception) {}
            val startArgs = JSONObject()
                .put("target_id", tid)
                .put("id", tid)
                .put("method", "virtualdisplay")
                .put("virtualDisplay", true)
            val started = capture.start(startArgs, caps.active)
            if (!started.optBoolean("ok", false)) {
                input.vdDisplayActive = false
                // Roll back so a pending MediaProjection consent still starts display:*.
                activeTargetId = if (prevTarget.startsWith("display:")) prevTarget else "display:0"
                appendLog("peer", "app sandbox failed: ${started.optString("error")} → rollback=$activeTargetId")
                return started.put("id", tid).put("peer_proto", 2)
            }
            pendingStartAfterConsent = false
            appendLog("peer", "app sandbox ok displayId=${started.optInt("displayId", -1)}")
            return started.put("id", tid).put("peer_proto", 2)
        }

        // display:* — whole screen MediaProjection; clear any VD sandbox.
        input.vdDisplayActive = false
        MimicAccessibilityService.clearConfine()
        try { caps.shizuku.stopSession() } catch (_: Exception) {}
        appendLog("peer", "display target — MediaProjection path")

        if (!capture.hasProjectionConsent()) {
            pendingStartAfterConsent = true
            main.post { requestProjection?.invoke() }
            return JSONObject()
                .put("ok", true)
                .put("id", tid)
                .put("pending_consent", true)
                .put("peer_proto", 2)
        }

        return startCaptureForActiveTarget(tid)
    }

    /** Start encoder for [tid] when consent exists (parity with PC cmd_set_stream_gate). */
    private fun startCaptureForActiveTarget(tid: String = activeTargetId): JSONObject {
        val startArgs = JSONObject().put("target_id", tid).put("id", tid)
        val started = capture.start(startArgs, caps.active)
        return if (started.optBoolean("ok", false)) {
            pendingStartAfterConsent = false
            appendLog("cap", "encoder started target=$tid")
            // Keep Mimic in foreground while sharing — moveTaskToBack caused OEM
            // to kill/suspend WebView + OkHttp WS (reconnect storm → LAN media die).
            // User can manually switch apps; CaptureService FGS keeps encoding alive.
            appendLog("cap", "display target streaming — stay foreground (no auto-minimize)")
            // One delayed IDR kick — PeerSession.onMediaLinkReady also requests once.
            // Avoid the old 4× storm (80/250/700/1600) that flooded TCP + decoder.
            main.postDelayed({
                try { capture.requestKeyframe() } catch (_: Exception) {}
            }, 200L)
            started.put("id", tid).put("peer_proto", 2)
        } else {
            JSONObject()
                .put("ok", false)
                .put("error", started.optString("error", "capture start failed"))
                .put("id", tid)
        }
    }

    fun onProjectionResult(resultCode: Int, data: Intent?) {
        capture.setProjectionResult(resultCode, data)
        val ok = resultCode != 0 && data != null
        appendLog("cap", if (ok) "MediaProjection granted" else "MediaProjection denied")
        // Only MediaProjection path uses this consent — never auto-start app:* here.
        val tid = activeTargetId
        if (!ok) pendingStartAfterConsent = false
        if (ok && tid.startsWith("app:")) {
            appendLog("cap", "consent for display but activeTargetId=$tid")
            main.post {
                pushToJs(
                    JSONObject()
                        .put("type", "projection_result")
                        .put("ok", true)
                        .put("started", false)
                        .put("target_id", tid)
                        .put("error", "consent for display but activeTargetId=$tid"),
                )
            }
            return
        }
        val shouldStart = ok && tid.startsWith("display:") &&
            (pendingStartAfterConsent || allowStream) && !capture.streaming
        if (!shouldStart) {
            main.post {
                pushToJs(
                    JSONObject()
                        .put("type", "projection_result")
                        .put("ok", ok)
                        .put("started", false)
                        .put("target_id", tid),
                )
            }
            return
        }
        // CRITICAL: Activity-result callback runs on the main thread. Starting
        // CaptureService.awaitForeground + MediaCodec.configure here froze the
        // whole app (and QQ share) on first MediaProjection consent.
        appendLog("cap", "defer encoder start off main thread target=$tid")
        io.execute {
            try {
                // Let Activity/WebView resume after the system consent UI.
                Thread.sleep(180)
            } catch (_: InterruptedException) {
            }
            val started = startCaptureForActiveTarget(tid)
            val startedOk = started.optBoolean("ok", false)
            val startError = if (startedOk) "" else started.optString("error", "capture start failed")
            if (!startedOk) appendLog("cap", "auto-start failed: $startError")
            else appendLog("cap", "auto-start after projection consent target=$tid")
            main.post {
                val msg = JSONObject()
                    .put("type", "projection_result")
                    .put("ok", true)
                    .put("started", startedOk)
                    .put("target_id", tid)
                if (startError.isNotBlank()) msg.put("error", startError)
                pushToJs(msg)
            }
        }
    }

    fun dispatch(cmd: String, args: JSONObject): Any {
        return when (cmd) {
            "get_version" -> readVersion()
            "show_window" -> jsonOk()
            "get_elevation" -> JSONObject().put("admin", false)
            "switch_permission" -> jsonErr("android: use set_capability_backend (normal/shizuku/root)")
            "get_capability_backend" -> caps.statusJson()
                .put("a11y_enabled", MimicAccessibilityService.isEnabled())
            "set_capability_backend" -> caps.setBackend(args.optString("backend", "normal"))
            "open_shizuku" -> openShizukuApp()
            "open_url" -> openExternalUrl(args.optString("url", ""))
            "crash_log" -> {
                appendLog("crash", "${args.optString("kind")} | ${args.optString("message")}")
                jsonOk()
            }
            "log_ui_event" -> {
                appendLog("ui", args.optString("event", ""))
                jsonOk()
            }
            "clipboard_write" -> {
                val text = args.optString("text", "")
                if (text.isEmpty()) return JSONObject().put("ok", false).put("error", "empty text")
                try {
                    val cm = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                    cm.setPrimaryClip(ClipData.newPlainText("mimic-log", text))
                    appendLog("log", "clipboard_write ok chars=${text.length}")
                    jsonOk().put("chars", text.length)
                } catch (e: Exception) {
                    JSONObject().put("ok", false).put("error", e.message ?: "clipboard failed")
                }
            }
            "share_live_log" -> {
                // Share live.log via FileProvider — never round-trip MBs through WebView.
                // Tail-read only; QQ cold-start freezes on huge text/plain streams.
                try {
                    val maxBytes = 256 * 1024
                    val fromFile = try {
                        if (logFile.exists()) readFileTail(logFile, maxBytes) else ""
                    } catch (_: Exception) {
                        ""
                    }
                    val fromRing = synchronized(ring) {
                        val joined = ring.joinToString("\n")
                        if (joined.length <= maxBytes) joined else joined.takeLast(maxBytes)
                    }
                    val content = if (fromFile.length >= fromRing.length) fromFile else fromRing
                    if (content.isEmpty()) {
                        return JSONObject().put("ok", false).put("error", "empty log")
                    }
                    val dir = File(context.cacheDir, "share").also { it.mkdirs() }
                    dir.listFiles()?.forEach { f ->
                        if (f.isFile && f.name.startsWith("mimic-log") &&
                            System.currentTimeMillis() - f.lastModified() > 86_400_000L
                        ) f.delete()
                    }
                    val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
                    val file = File(dir, "mimic-log_$stamp.txt")
                    file.writeText(content)
                    val uri = FileProvider.getUriForFile(
                        context,
                        "${context.packageName}.fileprovider",
                        file,
                    )
                    val send = Intent(Intent.ACTION_SEND).apply {
                        type = "text/plain"
                        putExtra(Intent.EXTRA_STREAM, uri)
                        putExtra(Intent.EXTRA_SUBJECT, "Mimic logs")
                        clipData = ClipData.newUri(context.contentResolver, "mimic-log", uri)
                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    }
                    // Activity context: do NOT set NEW_TASK (odd task stack → looks frozen until QQ killed).
                    val chooser = Intent.createChooser(send, "Mimic logs").apply {
                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    }
                    for (pkg in listOf(
                        "com.tencent.mobileqq",
                        "com.tencent.tim",
                        "com.tencent.mm",
                    )) {
                        try {
                            context.grantUriPermission(
                                pkg,
                                uri,
                                Intent.FLAG_GRANT_READ_URI_PERMISSION,
                            )
                        } catch (_: Exception) { /* not installed */ }
                    }
                    main.post {
                        try {
                            context.startActivity(chooser)
                        } catch (e: Exception) {
                            appendLog("log", "share_live_log startActivity: ${e.message}")
                        }
                    }
                    appendLog("log", "share_live_log ok path=${file.name} chars=${content.length}")
                    jsonOk().put("chars", content.length).put("file", file.name).put("mode", "file")
                } catch (e: Exception) {
                    JSONObject().put("ok", false).put("error", e.message ?: "share_live_log failed")
                }
            }
            "share_text" -> {
                val text = args.optString("text", "")
                if (text.isEmpty()) return JSONObject().put("ok", false).put("error", "empty text")
                // Default: share as .txt file (QQ/WeChat choke on long EXTRA_TEXT).
                val asFile = args.optBoolean("as_file", true)
                val rawName = args.optString("filename", "mimic-log.txt").ifBlank { "mimic-log.txt" }
                val safeName = rawName.replace(Regex("[^A-Za-z0-9._-]"), "_").take(64)
                try {
                    if (asFile) {
                        val dir = File(context.cacheDir, "share").also { it.mkdirs() }
                        dir.listFiles()?.forEach { f ->
                            if (f.isFile && f.name.startsWith("mimic-log") &&
                                System.currentTimeMillis() - f.lastModified() > 86_400_000L
                            ) f.delete()
                        }
                        val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
                        val file = File(dir, if (safeName.contains('.')) {
                            safeName.replace(".", "_$stamp.")
                        } else {
                            "${safeName}_$stamp.txt"
                        })
                        file.writeText(text)
                        val uri = FileProvider.getUriForFile(
                            context,
                            "${context.packageName}.fileprovider",
                            file,
                        )
                        val send = Intent(Intent.ACTION_SEND).apply {
                            type = "text/plain"
                            putExtra(Intent.EXTRA_STREAM, uri)
                            putExtra(Intent.EXTRA_SUBJECT, "Mimic logs")
                            clipData = ClipData.newUri(context.contentResolver, "mimic-log", uri)
                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        }
                        val chooser = Intent.createChooser(send, "Mimic logs").apply {
                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        }
                        for (pkg in listOf(
                            "com.tencent.mobileqq",
                            "com.tencent.tim",
                            "com.tencent.mm",
                        )) {
                            try {
                                context.grantUriPermission(
                                    pkg, uri, Intent.FLAG_GRANT_READ_URI_PERMISSION,
                                )
                            } catch (_: Exception) { }
                        }
                        main.post {
                            try {
                                context.startActivity(chooser)
                            } catch (e: Exception) {
                                appendLog("log", "share_file startActivity: ${e.message}")
                            }
                        }
                        appendLog("log", "share_file ok path=${file.name} chars=${text.length}")
                        jsonOk().put("chars", text.length).put("file", file.name).put("mode", "file")
                    } else {
                        val send = Intent(Intent.ACTION_SEND).apply {
                            type = "text/plain"
                            putExtra(Intent.EXTRA_TEXT, text.take(500_000))
                            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        }
                        main.post {
                            context.startActivity(
                                Intent.createChooser(send, "Mimic logs")
                                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                            )
                        }
                        appendLog("log", "share_text ok chars=${text.length}")
                        jsonOk().put("chars", text.length).put("mode", "text")
                    }
                } catch (e: Exception) {
                    JSONObject().put("ok", false).put("error", e.message ?: "share failed")
                }
            }
            "export_live_log" -> {
                // Full ring + live.log for share/copy — no WebView clipboard needed.
                val fromFile = try {
                    if (logFile.exists()) logFile.readText() else ""
                } catch (_: Exception) {
                    ""
                }
                val fromRing = synchronized(ring) { ring.joinToString("\n") }
                val content = if (fromFile.length >= fromRing.length) fromFile else fromRing
                JSONObject().put("ok", true).put("content", content).put("chars", content.length)
            }
            "read_live_log" -> JSONObject().put("lines", ring.joinToString("\n"))
            "read_logs" -> {
                val max = args.optInt("max_files", 20).coerceIn(1, 50)
                val files = logDir.listFiles { f ->
                    f.isFile && f.name.endsWith(".log") && f.name != "live.log"
                }?.sortedByDescending { it.lastModified() }?.take(max) ?: emptyList()
                val arr = JSONArray()
                for (f in files) {
                    arr.put(
                        JSONObject()
                            .put("name", f.name)
                            .put("size", f.length()),
                    )
                }
                JSONObject().put("files", arr)
            }
            "read_log_file" -> {
                val name = args.optString("filename", args.optString("name", ""))
                if (name.isBlank() || name.contains("..") || name.contains('/') || name.contains('\\')) {
                    JSONObject().put("ok", false).put("error", "invalid filename")
                } else {
                    val f = File(logDir, name)
                    if (!f.exists() || !f.isFile) {
                        JSONObject().put("ok", false).put("error", "file not found")
                    } else {
                        JSONObject()
                            .put("ok", true)
                            .put("filename", name)
                            .put("content", f.readText())
                    }
                }
            }
            "open_log_dir" -> {
                try {
                    val intent = Intent(Intent.ACTION_VIEW).setDataAndType(
                        Uri.parse(logDir.absolutePath),
                        "*/*",
                    ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    context.startActivity(intent)
                    jsonOk().put("dir", logDir.absolutePath)
                } catch (e: Exception) {
                    JSONObject()
                        .put("ok", true)
                        .put("dir", logDir.absolutePath)
                        .put("note", e.message ?: "no folder viewer")
                }
            }
            "clear_log" -> {
                ring.clear()
                logFile.writeText("")
                jsonOk()
            }
            "get_log_dir" -> JSONObject().put("dir", logDir.absolutePath)
            "get_settings" -> {
                val raw = prefs.getString("settings", null)
                val inner = if (raw.isNullOrBlank()) JSONObject() else JSONObject(raw)
                JSONObject().put("ok", true).put("settings", inner)
            }
            "set_settings" -> {
                val s = args.optJSONObject("settings") ?: args
                prefs.edit().putString("settings", s.toString()).apply()
                jsonOk()
            }
            "check_update" -> checkUpdate()
            "download_update" -> {
                io.execute {
                    try {
                        downloadAndPromptInstall()
                    } catch (e: Exception) {
                        Log.e(tag, "download_update", e)
                        pushProgress("error", e.message ?: "download failed")
                    }
                }
                jsonOk()
            }
            "clear_staging" -> {
                File(context.cacheDir, "updates").deleteRecursively()
                jsonOk()
            }
            "peer_probe" -> peerProbe(args.optString("url", DEFAULT_BOOTSTRAP))
            "peer_status" -> peer.statusJson()
            "peer_login", "peer_register", "peer_logout", "peer_invite", "peer_accept",
            "peer_reject", "peer_request_windows", "peer_set_target",
            "peer_send_control", "peer_set_control_mode", "peer_request_keyframe",
            "peer_list_devices",
            "peer_get_frame" -> peer.dispatch(cmd, args)
            "local_get_frame" -> {
                val fr = lastLocalPreview
                if (fr == null || fr.size < 16) {
                    JSONObject().put("ok", false).put("error", "no local preview")
                } else {
                    val w = java.nio.ByteBuffer.wrap(fr, 0, 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).int
                    val h = java.nio.ByteBuffer.wrap(fr, 4, 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).int
                    val flags = java.nio.ByteBuffer.wrap(fr, 8, 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).int
                    JSONObject()
                        .put("ok", true)
                        .put("w", w).put("h", h).put("flags", flags)
                        .put("b64", android.util.Base64.encodeToString(fr, android.util.Base64.NO_WRAP))
                }
            }
            "local_request_keyframe" -> {
                capture.requestKeyframe()
                jsonOk()
            }
            "peer_hangup" -> {
                allowStream = false
                acceptControl = false
                pendingStartAfterConsent = false
                input.vdDisplayActive = false
                lastLocalPreview = null
                try { caps.shizuku.stopSession() } catch (_: Exception) {}
                try { capture.stop() } catch (_: Exception) {}
                val r = peer.hangup()
                val end = JSONObject().put("type", "session_end").put("reason", "hangup")
                main.post { pushToJs(end) }
                pushGates()
                appendLog("peer", "hangup → gates closed")
                r
            }
            "request_projection" -> {
                main.post { requestProjection?.invoke() }
                jsonOk().put("pending", true)
            }
            "get_gates" -> JSONObject()
                .put("allow_stream", allowStream)
                .put("accept_control", acceptControl)
                .put("target_id", activeTargetId)
                .put("a11y_enabled", MimicAccessibilityService.isEnabled())
                .put("projection_consent", capture.hasProjectionConsent())
            "set_stream_gate" -> {
                allowStream = args.optBoolean("on", args.optBoolean("enabled", args.optInt("on", 0) != 0))
                appendLog("gate", "allow_stream=$allowStream")
                if (!allowStream) {
                    pendingStartAfterConsent = false
                    input.vdDisplayActive = false
                    try { caps.shizuku.stopSession() } catch (_: Exception) {}
                    capture.stop()
                    return@dispatch jsonOk().put("allow_stream", false)
                }
                // App sandbox uses Shizuku VD — no MediaProjection consent.
                if (activeTargetId.startsWith("app:")) {
                    input.vdDisplayActive = true
                    if (capture.streaming) {
                        return@dispatch jsonOk().put("allow_stream", true).put("streaming", true)
                    }
                    val startArgs = JSONObject()
                        .put("target_id", activeTargetId)
                        .put("id", activeTargetId)
                        .put("method", "virtualdisplay")
                        .put("virtualDisplay", true)
                    val started = capture.start(startArgs, caps.active)
                    if (!started.optBoolean("ok", false)) {
                        input.vdDisplayActive = false
                        return@dispatch JSONObject()
                            .put("ok", false)
                            .put("error", started.optString("error", "vd capture start failed"))
                            .put("allow_stream", true)
                    }
                    return@dispatch jsonOk().put("allow_stream", true).put("streaming", true)
                }
                if (!capture.hasProjectionConsent()) {
                    pendingStartAfterConsent = true
                    appendLog("gate", "MediaProjection consent not yet granted")
                    main.post { requestProjection?.invoke() }
                    return@dispatch JSONObject()
                        .put("ok", true)
                        .put("allow_stream", true)
                        .put("pending_consent", true)
                        .put("need_consent", true)
                }
                if (capture.streaming) {
                    return@dispatch jsonOk().put("allow_stream", true).put("streaming", true)
                }
                val started = startCaptureForActiveTarget(activeTargetId)
                if (!started.optBoolean("ok", false)) {
                    return@dispatch JSONObject()
                        .put("ok", false)
                        .put("error", started.optString("error", "capture start failed"))
                        .put("allow_stream", true)
                }
                jsonOk().put("allow_stream", true).put("streaming", true)
            }
            "set_control_gate" -> {
                acceptControl = args.optBoolean("on", args.optBoolean("enabled", args.optInt("on", 0) != 0))
                appendLog("gate", "accept_control=$acceptControl")
                if (acceptControl) {
                    if (!ShowTouchesHelper.ensurePermission(context)) {
                        appendLog("gate", "WRITE_SETTINGS needed for show_touches")
                    } else if (!ShowTouchesHelper.enableForSession(context)) {
                        appendLog("gate", "show_touches enable failed")
                    }
                } else {
                    ShowTouchesHelper.restore(context)
                }
                val a11y = MimicAccessibilityService.isEnabled()
                if (acceptControl && !a11y && !input.vdDisplayActive) {
                    appendLog("gate", "AccessibilityService not enabled")
                    main.post {
                        pushToJs(
                            JSONObject()
                                .put("type", "peer_error")
                                .put("error", "android: enable AccessibilityService for Main display control"),
                        )
                    }
                }
                jsonOk().put("accept_control", acceptControl).put("a11y_enabled", a11y)
            }
            "set_exclude_self" -> jsonOk() // N/A on Android for now
            "get_agent_status" -> JSONObject().put("connected", false)
            "get_server_status" -> JSONObject().put("connected", false)
            "screen_info" -> JSONObject()
                .put("x", 0).put("y", 0)
                .put("w", context.resources.displayMetrics.widthPixels)
                .put("h", context.resources.displayMetrics.heightPixels)
            "list_targets" -> listTargets()
            "list_windows" -> listTargetsAsWindowsCompat()
            "list_processes", "list_desktops" -> JSONArray()
            "launch_app" -> {
                val pkg = args.optString("packageName", args.optString("package", ""))
                val act = args.optString("activity", "")
                if (pkg.isBlank()) jsonErr("missing packageName")
                else AppLauncher.launch(context, pkg, act.ifBlank { null })
            }
            "capture_stream_start" -> {
                if (!allowStream) jsonErr("allow_stream gate closed")
                else {
                    val tid = args.optString("target_id", args.optString("id", activeTargetId))
                    if (tid.isNotBlank()) activeTargetId = tid
                    if (activeTargetId.startsWith("app:")) {
                        input.vdDisplayActive = true
                        val startArgs = JSONObject(args.toString())
                            .put("target_id", activeTargetId)
                            .put("id", activeTargetId)
                            .put("method", "virtualdisplay")
                            .put("virtualDisplay", true)
                        capture.start(startArgs, caps.active)
                    } else if (!capture.hasProjectionConsent()) {
                        pendingStartAfterConsent = true
                        main.post { requestProjection?.invoke() }
                        JSONObject()
                            .put("ok", false)
                            .put("error", "android: MediaProjection consent required")
                            .put("need_consent", true)
                    } else {
                        capture.start(args, caps.active)
                    }
                }
            }
            "capture_stream_stop" -> capture.stop()
            "capture_window" -> jsonErr("android: single-frame capture not implemented yet")
            "send_input" -> {
                if (!acceptControl) jsonErr("accept_control gate closed")
                else input.inject(args, caps.active)
            }
            else -> jsonErr("android: unsupported cmd '$cmd'")
        }
    }

    /** v2 targets: main display + installed launchable apps. */
    private fun listTargets(): JSONObject {
        val dm = context.resources.displayMetrics
        val priv = caps.canVirtualDisplay()
        val targets = JSONArray()
        targets.put(
            TargetDescriptor(
                id = "display:0",
                kind = "display",
                title = "Main Display",
                displayId = 0,
                hwnd = 0,
                capture = true,
                control = true,
                launch = false,
                virtualDisplay = false,
            ).toJson()
                .put("w", dm.widthPixels)
                .put("h", dm.heightPixels)
        )
        for (app in AppEnumerator.listLaunchable(context)) {
            targets.put(
                app.copy(
                    capture = priv,
                    control = priv,
                    launch = true,
                    virtualDisplay = priv,
                ).toJson()
            )
        }
        return JSONObject()
            .put("ok", true)
            .put("targets", targets)
            .put("peer_proto", 2)
            .put("virtual_display_ready", priv)
    }

    /** Temporary Windows-shaped array so existing TargetPicker does not crash. */
    private fun listTargetsAsWindowsCompat(): JSONArray {
        val arr = JSONArray()
        arr.put(
            JSONObject()
                .put("title", " Main Display")
                .put("category", "desktop")
                .put("hwnd", 0)
                .put("id", "display:0")
                .put("platform", "android")
                .put("kind", "display")
        )
        // Apps as window-like entries (hwnd = stable hash; real id in id field)
        for (app in AppEnumerator.listLaunchable(context)) {
            arr.put(
                JSONObject()
                    .put("title", app.title)
                    .put("category", "window")
                    .put("hwnd", app.id.hashCode().toLong() and 0x7fffffff)
                    .put("id", app.id)
                    .put("platform", "android")
                    .put("kind", "app")
                    .put("packageName", app.packageName)
            )
        }
        return arr
    }

    private fun checkUpdate(): JSONObject {
        val local = readVersion()
        val manifest = httpGetJson(CDN_VERSION) ?: return JSONObject()
            .put("ok", false)
            .put("error", "version.json unreachable")
            .put("current", local)
        val remote = manifest.optString("app", "")
        val apkName = manifest.optString("client_apk", manifest.optString("apk", ""))
        val base = manifest.optString("download_base", CDN_BASE).trimEnd('/') + "/"
        val has = remote.isNotBlank() && compareSemver(remote, local) > 0
        // Platform-scoped update SSOT — never emit PC jump_pad / Windows versions.
        val o = JSONObject()
            .put("ok", true)
            .put("platform", "android")
            .put("current", local)
            .put("latest", if (remote.isNotBlank()) remote else local)
            .put("has_update", has)
            .put("mode", "full")
            .put("jump_pad", "") // empty: UpdateModal must not apply PC 0.3.31 ladder
            .put("message", "")
            .put("name", "")
            .put("body", "")
            .put("download_base", base)
        if (has && apkName.isNotBlank()) {
            val size = manifest.optLong("client_size", 0).takeIf { it > 0 }
                ?: tryHeadSize(base + apkName)
            val sha = manifest.optString("client_sha256", "")
            val entry = JSONObject()
                .put("path", apkName)
                .put("size", size)
                .put("dl", size)
            if (sha.isNotBlank()) entry.put("sha256", sha)
            o.put("diff", JSONArray().put(entry))
        } else {
            o.put("diff", JSONArray())
        }
        return o
    }

    private fun downloadAndPromptInstall() {
        val manifest = httpGetJson(CDN_VERSION) ?: throw Exception("version.json unreachable")
        val base = manifest.optString("download_base", CDN_BASE).trimEnd('/') + "/"
        val apkName = manifest.optString("client_apk", manifest.optString("apk", ""))
        if (apkName.isBlank()) throw Exception("client_apk missing")
        val expectSha = manifest.optString("client_sha256", "").lowercase()
        val expectSize = manifest.optLong("client_size", 0).takeIf { it > 0 }
            ?: tryHeadSize(base + apkName)
        val url = base + apkName
        val dir = File(context.cacheDir, "updates").apply { mkdirs() }
        val out = File(dir, apkName)
        // Seed progress with known total so UI starts on a byte bar (not file-count).
        pushBytes(apkName, 0L, expectSize.coerceAtLeast(1L))
        httpDownload(url, out) { read, total ->
            val tot = when {
                total > 0L -> total
                expectSize > 0L -> expectSize
                else -> read.coerceAtLeast(1L)
            }
            pushBytes(apkName, read, tot)
        }
        if (expectSha.isNotBlank()) {
            val actual = sha256File(out)
            if (!actual.equals(expectSha, ignoreCase = true)) {
                out.delete()
                pushProgress("error", "sha256 mismatch")
                throw Exception("apk sha256 mismatch (got $actual)")
            }
            appendLog("update", "apk sha256 ok")
        }
        // Final 100% before installer prompt.
        pushBytes(apkName, out.length(), out.length().coerceAtLeast(1L))
        pushProgress("done", apkName, doneBytes = out.length(), totalBytes = out.length())
        main.post { promptInstall(out) }
    }

    private fun promptInstall(apk: File) {
        if (Build.VERSION.SDK_INT >= 26 &&
            !context.packageManager.canRequestPackageInstalls()
        ) {
            val i = Intent(
                android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                Uri.parse("package:${context.packageName}")
            ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(i)
        }
        val uri = FileProvider.getUriForFile(
            context,
            "${context.packageName}.fileprovider",
            apk
        )
        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        context.startActivity(intent)
    }

    /** Match PC peer_probe: ok+rtt_ms on success, ok:false on unreachable (铁律 5). */
    private fun peerProbe(baseUrl: String): JSONObject {
        val base = baseUrl.trimEnd('/')
        if (base.isEmpty()) {
            return JSONObject().put("ok", false).put("error", "empty url")
        }
        val t0 = android.os.SystemClock.elapsedRealtime()
        val health = try {
            httpGetJson("$base/health")
        } catch (_: Exception) {
            null
        }
        val ms = (android.os.SystemClock.elapsedRealtime() - t0).toInt().coerceAtLeast(0)
        if (health == null || !health.optBoolean("ok", true)) {
            return JSONObject().put("ok", false).put("error", "unreachable")
        }
        val cluster = try {
            httpGetJson("$base/api/cluster")
        } catch (_: Exception) {
            null
        }
        val nodeCount = when {
            cluster?.has("nodeCount") == true -> cluster.getInt("nodeCount")
            cluster?.optJSONArray("nodes") != null -> cluster.getJSONArray("nodes").length()
            health.has("nodeCount") -> health.getInt("nodeCount")
            else -> 1
        }.coerceAtLeast(1)
        return JSONObject()
            .put("ok", true)
            .put("url", base)
            .put("reachable", true)
            .put("rtt_ms", ms)
            .put("node_count", nodeCount)
            .put("role", health.optString("role", ""))
            .put("instanceId", health.optString("instanceId", ""))
    }

    /** Read last maxBytes of a UTF-8 text file without loading the whole file. */
    private fun readFileTail(file: File, maxBytes: Int): String {
        val len = file.length()
        if (len <= 0L) return ""
        FileInputStream(file).use { fis ->
            if (len > maxBytes) {
                fis.skip(len - maxBytes)
            }
            val buf = ByteArray(minOf(len, maxBytes.toLong()).toInt())
            var off = 0
            while (off < buf.size) {
                val n = fis.read(buf, off, buf.size - off)
                if (n <= 0) break
                off += n
            }
            // Drop possible partial UTF-8 lead byte at start after skip.
            var start = 0
            if (len > maxBytes) {
                while (start < off && (buf[start].toInt() and 0xC0) == 0x80) start++
            }
            return String(buf, start, off - start, Charsets.UTF_8)
        }
    }

    private fun appendLog(tagName: String, msg: String) {
        val ts = SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(Date())
        val line = "[$ts] [$tagName] $msg"
        synchronized(ring) {
            if (ring.size >= 2000) ring.removeFirst()
            ring.addLast(line)
        }
        try {
            logFile.appendText(line + "\n")
        } catch (_: Exception) {
        }
        Log.i(tag, msg)
        // Push to shared/web LogManager (same shape as WebView2)
        val push = JSONObject()
            .put("type", "log")
            .put("ts", ts)
            .put("tag", tagName)
            .put("msg", msg)
            .put("count", 1)
        main.post { pushToJs(push) }
    }

    private fun pushProgress(
        phase: String,
        file: String = "",
        doneFiles: Int = 0,
        totalFiles: Int = 1,
        doneBytes: Long = 0L,
        totalBytes: Long = 0L,
    ) {
        val o = JSONObject()
            .put("type", "update_progress")
            .put("phase", phase)
            .put("current_file", doneFiles)
            .put("total_files", totalFiles)
            .put("skipped_files", 0)
            .put("file", file)
            .put("done_bytes", doneBytes)
            .put("total_bytes", totalBytes)
            .put("skipped_bytes", 0)
        if (phase == "error") o.put("error_file", file)
        main.post { pushToJs(o) }
    }

    private fun pushBytes(file: String, done: Long, total: Long) {
        val o = JSONObject()
            .put("type", "update_progress")
            .put("phase", "download")
            .put("current_file", 1)
            .put("total_files", 1)
            .put("skipped_files", 0)
            .put("file", file)
            .put("done_bytes", done)
            .put("total_bytes", total.coerceAtLeast(done).coerceAtLeast(1L))
            .put("skipped_bytes", 0)
        main.post { pushToJs(o) }
    }

    private fun readVersion(): String {
        return try {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "0.0.0"
        } catch (_: Exception) {
            "0.0.0"
        }
    }

    /** Open http(s) URL in the system browser — never navigate the app WebView. */
    private fun openExternalUrl(url: String): JSONObject {
        val trimmed = url.trim()
        if (trimmed.isEmpty()) return jsonErr("empty url")
        val uri = try {
            Uri.parse(trimmed)
        } catch (e: Exception) {
            return jsonErr(e.message ?: "invalid url")
        }
        val scheme = (uri.scheme ?: "").lowercase()
        if (scheme != "http" && scheme != "https") {
            return jsonErr("only http(s) urls allowed")
        }
        return try {
            val intent = Intent(Intent.ACTION_VIEW, uri).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(intent)
            appendLog("ui", "open_url $trimmed")
            jsonOk()
        } catch (e: Exception) {
            jsonErr(e.message ?: "startActivity failed")
        }
    }

    /** Launch Shizuku manager (or Play Store / official site fallback). */
    private fun openShizukuApp(): JSONObject {
        val pm = context.packageManager
        val pkgs = listOf("moe.shizuku.privileged.api", "moe.shizuku.manager")
        for (pkg in pkgs) {
            val launch = pm.getLaunchIntentForPackage(pkg)
            if (launch != null) {
                return try {
                    launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    context.startActivity(launch)
                    JSONObject().put("ok", true).put("package", pkg)
                } catch (e: Exception) {
                    JSONObject().put("ok", false).put("error", e.message ?: "startActivity failed")
                }
            }
        }
        return try {
            val market = Intent(
                Intent.ACTION_VIEW,
                Uri.parse("market://details?id=moe.shizuku.privileged.api"),
            ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(market)
            JSONObject().put("ok", true).put("via", "market")
        } catch (_: Exception) {
            try {
                val web = Intent(
                    Intent.ACTION_VIEW,
                    Uri.parse("https://shizuku.rikka.app/"),
                ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                context.startActivity(web)
                JSONObject().put("ok", true).put("via", "web")
            } catch (e: Exception) {
                JSONObject().put("ok", false).put("error", "android: Shizuku not installed (${e.message})")
            }
        }
    }

    private fun jsonOk() = JSONObject().put("ok", true)
    private fun jsonErr(msg: String) = JSONObject().put("ok", false).put("error", msg)

    private fun httpGetJson(url: String): JSONObject? {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 10000
            readTimeout = 15000
            requestMethod = "GET"
        }
        return try {
            if (conn.responseCode !in 200..299) return null
            JSONObject(conn.inputStream.bufferedReader().use { it.readText() })
        } finally {
            conn.disconnect()
        }
    }

    private fun tryHeadSize(url: String): Long {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 8000
            readTimeout = 8000
            requestMethod = "HEAD"
        }
        return try {
            if (conn.responseCode in 200..299) conn.contentLengthLong.coerceAtLeast(0) else 0
        } catch (_: Exception) {
            0
        } finally {
            conn.disconnect()
        }
    }

    private fun sha256File(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { input ->
            val buf = ByteArray(64 * 1024)
            while (true) {
                val n = input.read(buf)
                if (n <= 0) break
                digest.update(buf, 0, n)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    private fun httpDownload(url: String, dest: File, onProgress: (Long, Long) -> Unit) {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15000
            readTimeout = 180000
            requestMethod = "GET"
        }
        try {
            if (conn.responseCode !in 200..299) throw Exception("HTTP ${conn.responseCode}")
            val total = conn.contentLengthLong
            conn.inputStream.use { input ->
                FileOutputStream(dest).use { output ->
                    val buf = ByteArray(64 * 1024)
                    var readTotal = 0L
                    while (true) {
                        val n = input.read(buf)
                        if (n <= 0) break
                        output.write(buf, 0, n)
                        readTotal += n
                        onProgress(readTotal, total)
                    }
                }
            }
        } finally {
            conn.disconnect()
        }
    }

    companion object {
        const val DEFAULT_BOOTSTRAP = "http://47.107.43.5:8443"
        const val CDN_BASE = "http://47.107.43.5/mimic/android/"
        const val CDN_VERSION = "http://47.107.43.5/mimic/android/version.json"

        fun compareSemver(a: String, b: String): Int {
            fun parts(s: String) = s.trim().removePrefix("v").split('.').map { it.toIntOrNull() ?: 0 }
            val pa = parts(a)
            val pb = parts(b)
            val n = maxOf(pa.size, pb.size)
            for (i in 0 until n) {
                val x = pa.getOrElse(i) { 0 }
                val y = pb.getOrElse(i) { 0 }
                if (x != y) return x - y
            }
            return 0
        }
    }
}
