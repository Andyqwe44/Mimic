package com.mimic.setup

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.FileProvider
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

/**
 * Thin installer (PC Setup.exe analogue):
 *   1) GET CDN version.json
 *   2) Download client APK from download_base
 *   3) Prompt PackageInstaller / ACTION_VIEW
 */
class MainActivity : AppCompatActivity() {

    private val io = Executors.newSingleThreadExecutor()
    private lateinit var status: TextView
    private lateinit var detail: TextView
    private lateinit var progress: ProgressBar
    private lateinit var btn: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        status = findViewById(R.id.status)
        detail = findViewById(R.id.detail)
        progress = findViewById(R.id.progress)
        btn = findViewById(R.id.btnInstall)

        btn.setOnClickListener { startInstall() }
        startInstall()
    }

    private fun startInstall() {
        btn.isEnabled = false
        progress.isIndeterminate = true
        setUi("Connecting to CDN…", CDN_VERSION)

        if (Build.VERSION.SDK_INT >= 26 && !packageManager.canRequestPackageInstalls()) {
            setUi(
                "Allow install from this app",
                "Android requires permission to install APKs downloaded by Setup."
            )
            startActivity(
                Intent(
                    Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:$packageName")
                )
            )
            btn.isEnabled = true
            progress.isIndeterminate = false
            return
        }

        io.execute {
            try {
                val manifest = httpGetJson(CDN_VERSION)
                    ?: throw Exception("Cannot reach $CDN_VERSION")
                val base = manifest.optString("download_base", CDN_BASE).trimEnd('/') + "/"
                val apkName = when {
                    manifest.has("client_apk") && manifest.getString("client_apk").isNotBlank() ->
                        manifest.getString("client_apk")
                    manifest.has("apk") && manifest.getString("apk").isNotBlank() ->
                        manifest.getString("apk")
                    else -> throw Exception("version.json missing client_apk / apk")
                }
                val url = base + apkName
                runOnUiThread {
                    setUi("Downloading Mimic Client…", url)
                    progress.isIndeterminate = false
                    progress.progress = 0
                    progress.max = 100
                }
                val dir = File(cacheDir, "cdn").apply { mkdirs() }
                val out = File(dir, apkName)
                httpDownload(url, out) { read, total ->
                    if (total > 0) {
                        val pct = ((read * 100) / total).toInt().coerceIn(0, 100)
                        runOnUiThread { progress.progress = pct }
                    }
                }
                runOnUiThread {
                    setUi("Install Mimic Client", "Downloaded ${out.length()} bytes — opening installer…")
                    promptInstall(out)
                    btn.isEnabled = true
                    btn.text = getString(R.string.retry)
                }
            } catch (e: Exception) {
                runOnUiThread {
                    setUi("Install failed", e.message ?: "unknown error")
                    btn.isEnabled = true
                    btn.text = getString(R.string.retry)
                    progress.isIndeterminate = false
                }
            }
        }
    }

    private fun promptInstall(apk: File) {
        val uri = FileProvider.getUriForFile(
            this,
            "$packageName.fileprovider",
            apk
        )
        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        startActivity(intent)
    }

    private fun setUi(title: String, body: String) {
        status.text = title
        detail.text = body
    }

    private fun httpGetJson(url: String): JSONObject? {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 10000
            readTimeout = 15000
            requestMethod = "GET"
        }
        return try {
            if (conn.responseCode !in 200..299) return null
            val body = conn.inputStream.bufferedReader().use { it.readText() }
            JSONObject(body)
        } finally {
            conn.disconnect()
        }
    }

    private fun httpDownload(url: String, dest: File, onProgress: (Long, Long) -> Unit) {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15000
            readTimeout = 180000
            requestMethod = "GET"
        }
        try {
            if (conn.responseCode !in 200..299) {
                throw Exception("HTTP ${conn.responseCode} for $url")
            }
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
            if (dest.length() < 1024) {
                throw Exception("Downloaded file too small (${dest.length()} B) — is client APK on CDN?")
            }
        } finally {
            conn.disconnect()
        }
    }

    companion object {
        const val CDN_BASE = "http://47.107.43.5/mimic/android/"
        const val CDN_VERSION = "http://47.107.43.5/mimic/android/version.json"
    }
}
