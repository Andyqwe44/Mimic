package com.mimic.client

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
    private val logFile = File(context.filesDir, "live.log")
    private val ring = ArrayDeque<String>(500)

    // Gate SSOT — UI may open gates, but stream/control still fail until backends exist.
    @Volatile private var allowStream = false
    @Volatile private var acceptControl = false
    @Volatile private var activeTargetId: String = "display:0"
    /** After MediaProjection consent, start capture if stream gate / set_target requested it. */
    @Volatile private var pendingStartAfterConsent = false
    private val caps = CapabilityManager(context)
    private val capture = CaptureController(context)
    private val input = InputController(context)
    private val peer = PeerSession(context, pushToJs)
    /** Set by MainActivity to launch the system screen-capture consent dialog. */
    var requestProjection: (() -> Unit)? = null

    init {
        capture.onEncodedFrame = { packed -> peer.sendH264Packed(packed) }
        peer.onControlAction = { action ->
            if (acceptControl) input.inject(action, caps.active)
            else JSONObject().put("ok", false).put("error", "accept_control gate closed")
        }
        peer.onListTargets = { listTargets() }
        peer.onSetTarget = { json -> applyRemoteSetTarget(json) }
        peer.onRequestKeyframe = { capture.requestKeyframe() }
    }

    /**
     * Controller picked a remote target — apply on controlled Android (parity with PC).
     * Auto-opens stream/control gates; starts capture when MediaProjection consent exists.
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
        activeTargetId = tid
        appendLog("peer", "set_target id=$tid")

        // Launch app targets so the controlled device shows the chosen app.
        if (tid.startsWith("app:")) {
            val rest = tid.removePrefix("app:")
            val slash = rest.indexOf('/')
            val pkg = if (slash >= 0) rest.substring(0, slash) else rest
            val act = if (slash >= 0) rest.substring(slash + 1) else ""
            if (pkg.isNotBlank()) {
                val launch = AppLauncher.launch(context, pkg, act.ifBlank { null })
                if (!launch.optBoolean("ok", false)) {
                    appendLog("peer", "set_target launch failed: ${launch.optString("error")}")
                }
            }
        }

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
        var startedOk = false
        var startError = ""
        if (ok && (pendingStartAfterConsent || allowStream) && !capture.streaming) {
            val started = startCaptureForActiveTarget(activeTargetId)
            startedOk = started.optBoolean("ok", false)
            if (!startedOk) startError = started.optString("error", "capture start failed")
            else appendLog("cap", "auto-start after projection consent")
        }
        if (!ok) pendingStartAfterConsent = false
        main.post {
            val msg = JSONObject()
                .put("type", "projection_result")
                .put("ok", ok)
                .put("started", startedOk)
            if (startError.isNotBlank()) msg.put("error", startError)
            pushToJs(msg)
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
            "crash_log" -> {
                appendLog("crash", "${args.optString("kind")} | ${args.optString("message")}")
                jsonOk()
            }
            "log_ui_event" -> {
                appendLog("ui", args.optString("event", ""))
                jsonOk()
            }
            "read_live_log" -> JSONObject().put("lines", ring.joinToString("\n"))
            "read_logs" -> JSONObject().put("files", JSONArray())
            "clear_log" -> {
                ring.clear()
                logFile.writeText("")
                jsonOk()
            }
            "get_log_dir" -> JSONObject().put("dir", context.filesDir.absolutePath)
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
            "peer_reject", "peer_hangup", "peer_request_windows", "peer_set_target",
            "peer_send_control", "peer_set_control_mode", "peer_request_keyframe",
            "peer_get_frame" -> peer.dispatch(cmd, args)
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
                    capture.stop()
                    return@dispatch jsonOk().put("allow_stream", false)
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
                if (acceptControl && !MimicAccessibilityService.isEnabled()) {
                    appendLog("gate", "AccessibilityService not enabled")
                }
                jsonOk().put("accept_control", acceptControl)
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
                    if (!capture.hasProjectionConsent()) {
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
        val targets = JSONArray()
        targets.put(
            TargetDescriptor(
                id = "display:0",
                kind = "display",
                title = "Main Display",
                displayId = 0,
                hwnd = 0,
                capture = false,
                control = false,
                launch = false,
                virtualDisplay = false,
            ).toJson()
                .put("w", dm.widthPixels)
                .put("h", dm.heightPixels)
        )
        for (app in AppEnumerator.listLaunchable(context)) {
            targets.put(app.toJson())
        }
        return JSONObject().put("ok", true).put("targets", targets).put("peer_proto", 2)
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
            .put("message", manifest.optString("message", ""))
            .put("name", "Mimic Android $remote")
            .put("body", "APK update from CDN android/ (not mimic/client)")
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
            o.put("body", "Full APK replace (Android PackageInstaller — not PC multi-file incremental)")
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
        val url = base + apkName
        val dir = File(context.cacheDir, "updates").apply { mkdirs() }
        val out = File(dir, apkName)
        pushProgress("download", apkName, 0, 1)
        httpDownload(url, out) { read, total ->
            if (total > 0) {
                pushBytes(apkName, read, total)
            }
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
        pushProgress("done", apkName)
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

    private fun appendLog(tagName: String, msg: String) {
        val ts = SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(Date())
        val line = "[$ts] [$tagName] $msg"
        synchronized(ring) {
            if (ring.size >= 500) ring.removeFirst()
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

    private fun pushProgress(phase: String, file: String = "", doneFiles: Int = 0, totalFiles: Int = 1) {
        val o = JSONObject()
            .put("type", "update_progress")
            .put("phase", phase)
            .put("current_file", doneFiles)
            .put("total_files", totalFiles)
            .put("skipped_files", 0)
            .put("file", file)
            .put("done_bytes", 0)
            .put("total_bytes", 0)
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
            .put("total_bytes", total)
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
