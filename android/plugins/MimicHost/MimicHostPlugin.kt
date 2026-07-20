package com.mimic.client.plugins

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.core.content.FileProvider
import com.getcapacitor.JSObject
import com.getcapacitor.Plugin
import com.getcapacitor.PluginCall
import com.getcapacitor.PluginMethod
import com.getcapacitor.annotation.CapacitorPlugin
import org.json.JSONObject
import java.io.BufferedReader
import java.io.File
import java.io.FileOutputStream
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

/**
 * Native bridge for shared/web hostCall — mirrors Windows WebView2 commands used
 * by the Android skeleton (crash log, signaling HTTP, APK update).
 *
 * After `npx cap add android`, register this plugin in MainActivity and copy
 * this file under app/src/main/java/com/mimic/client/plugins/.
 */
@CapacitorPlugin(name = "MimicHost")
class MimicHostPlugin : Plugin() {

    private val io = Executors.newSingleThreadExecutor()
    private val tag = "MimicHost"
    private val prefs by lazy {
        context.getSharedPreferences("mimic_host", Context.MODE_PRIVATE)
    }

    @PluginMethod
    fun call(call: PluginCall) {
        val cmd = call.getString("cmd") ?: run {
            call.reject("missing cmd")
            return
        }
        val args = call.getObject("args") ?: JSObject()
        io.execute {
            try {
                val result = dispatch(cmd, args)
                val envelope = JSObject()
                envelope.put("result", result)
                call.resolve(envelope)
            } catch (e: Exception) {
                Log.e(tag, "cmd=$cmd failed", e)
                call.reject(e.message ?: "error")
            }
        }
    }

    private fun dispatch(cmd: String, args: JSObject): Any {
        return when (cmd) {
            "get_version" -> "\"${readVersion()}\""
            "crash_log" -> {
                val kind = args.getString("kind") ?: "error"
                val message = args.getString("message") ?: ""
                Log.e(tag, "CRASH $kind | $message")
                appendCrashFile(kind, message)
                jsonOk()
            }
            "log_ui_event" -> {
                Log.i(tag, "[ui] ${args.getString("event") ?: ""}")
                jsonOk()
            }
            "read_live_log" -> {
                val o = JSObject()
                o.put("lines", readCrashTail())
                o
            }
            "get_settings" -> {
                val raw = prefs.getString("settings", "{}") ?: "{}"
                JSObject(raw)
            }
            "set_settings" -> {
                val settings = args.getJSObject("settings") ?: args
                prefs.edit().putString("settings", settings.toString()).apply()
                jsonOk()
            }
            "peer_probe" -> peerProbe(args.getString("url") ?: DEFAULT_BOOTSTRAP)
            "check_update" -> checkUpdate()
            "download_update" -> {
                downloadAndPromptInstall()
                jsonOk()
            }
            "show_window" -> jsonOk()
            "open_url" -> openExternalUrl(args.getString("url") ?: "")
            else -> {
                val o = JSObject()
                o.put("ok", false)
                o.put("error", "android skeleton: unsupported cmd '$cmd'")
                o
            }
        }
    }

    private fun peerProbe(baseUrl: String): JSObject {
        val health = httpGetJson("$baseUrl/health")
        val cluster = try {
            httpGetJson("$baseUrl/api/cluster")
        } catch (_: Exception) {
            null
        }
        val o = JSObject()
        o.put("ok", true)
        o.put("url", baseUrl)
        o.put("reachable", true)
        if (health != null) {
            o.put("role", health.optString("role", ""))
            o.put("instanceId", health.optString("instanceId", ""))
        }
        val nodeCount = when {
            cluster?.has("nodeCount") == true -> cluster.getInt("nodeCount")
            cluster?.optJSONArray("nodes") != null -> cluster.getJSONArray("nodes").length()
            health?.has("nodeCount") == true -> health.getInt("nodeCount")
            else -> 0
        }
        o.put("node_count", nodeCount)
        return o
    }

    private fun checkUpdate(): JSObject {
        val manifest = httpGetJson(CDN_VERSION) ?: throw Exception("version.json unreachable")
        val remote = manifest.optString("app", "")
        val local = readVersion()
        val o = JSObject()
        o.put("ok", true)
        o.put("local", local)
        o.put("remote", remote)
        o.put("update_available", compareSemver(remote, local) > 0)
        o.put("apk", manifest.optString("apk", ""))
        o.put("download_base", manifest.optString("download_base", CDN_BASE))
        o.put("message", manifest.optString("message", ""))
        return o
    }

    private fun downloadAndPromptInstall() {
        val manifest = httpGetJson(CDN_VERSION) ?: throw Exception("version.json unreachable")
        val base = manifest.optString("download_base", CDN_BASE).trimEnd('/')
        val apkName = manifest.optString("apk", "")
        if (apkName.isEmpty()) throw Exception("apk field missing in version.json")
        val url = "$base/$apkName"
        val dir = File(context.cacheDir, "updates").apply { mkdirs() }
        val out = File(dir, apkName)
        httpDownload(url, out)
        promptInstall(out)
    }

    private fun promptInstall(apk: File) {
        val uri: Uri = if (Build.VERSION.SDK_INT >= 24) {
            FileProvider.getUriForFile(context, context.packageName + ".fileprovider", apk)
        } else {
            Uri.fromFile(apk)
        }
        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        context.startActivity(intent)
    }

    private fun appendCrashFile(kind: String, message: String) {
        val f = File(context.filesDir, "crash.log")
        f.appendText("${System.currentTimeMillis()} [$kind] $message\n")
    }

    private fun readCrashTail(): String {
        val f = File(context.filesDir, "crash.log")
        if (!f.exists()) return ""
        val lines = f.readLines()
        return lines.takeLast(200).joinToString("\n")
    }

    private fun readVersion(): String {
        return try {
            val ai = context.packageManager.getPackageInfo(context.packageName, 0)
            ai.versionName ?: "0.1.0"
        } catch (_: Exception) {
            "0.1.0"
        }
    }

    private fun openExternalUrl(url: String): JSObject {
        val trimmed = url.trim()
        if (trimmed.isEmpty()) {
            val o = JSObject()
            o.put("ok", false)
            o.put("error", "empty url")
            return o
        }
        val uri = Uri.parse(trimmed)
        val scheme = (uri.scheme ?: "").lowercase()
        if (scheme != "http" && scheme != "https") {
            val o = JSObject()
            o.put("ok", false)
            o.put("error", "only http(s) urls allowed")
            return o
        }
        return try {
            val intent = Intent(Intent.ACTION_VIEW, uri).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(intent)
            jsonOk()
        } catch (e: Exception) {
            val o = JSObject()
            o.put("ok", false)
            o.put("error", e.message ?: "startActivity failed")
            o
        }
    }

    private fun jsonOk(): JSObject {
        val o = JSObject()
        o.put("ok", true)
        return o
    }

    private fun httpGetJson(url: String): JSONObject? {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 8000
            readTimeout = 8000
            requestMethod = "GET"
        }
        return try {
            if (conn.responseCode !in 200..299) return null
            val body = BufferedReader(InputStreamReader(conn.inputStream)).use { it.readText() }
            JSONObject(body)
        } finally {
            conn.disconnect()
        }
    }

    private fun httpDownload(url: String, dest: File) {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15000
            readTimeout = 120000
            requestMethod = "GET"
        }
        try {
            if (conn.responseCode !in 200..299) {
                throw Exception("HTTP ${conn.responseCode} for $url")
            }
            conn.inputStream.use { input ->
                FileOutputStream(dest).use { output -> input.copyTo(output) }
            }
        } finally {
            conn.disconnect()
        }
    }

    /** Positive if a > b (simple dotted numeric). */
    private fun compareSemver(a: String, b: String): Int {
        fun parts(s: String) = s.trim().split('.').map { it.toIntOrNull() ?: 0 }
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

    companion object {
        const val DEFAULT_BOOTSTRAP = "http://47.107.43.5:8443"
        const val CDN_BASE = "http://47.107.43.5/mimic/android/"
        const val CDN_VERSION = "http://47.107.43.5/mimic/android/version.json"
    }
}
