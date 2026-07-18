package com.mimic.client

import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

/**
 * Temporary thin client until Capacitor shared/web shell is wired.
 * Proves CDN → Setup → install → run, and Bootstrap reachability.
 */
class MainActivity : AppCompatActivity() {

    private val io = Executors.newSingleThreadExecutor()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        val status = findViewById<TextView>(R.id.status)
        val detail = findViewById<TextView>(R.id.detail)
        val btn = findViewById<Button>(R.id.btnProbe)

        status.text = getString(R.string.hello, BuildConfig.VERSION_NAME)
        detail.text = getString(R.string.stub_hint)

        btn.setOnClickListener {
            btn.isEnabled = false
            detail.text = "Probing $BOOTSTRAP …"
            io.execute {
                try {
                    val health = httpGetJson("$BOOTSTRAP/health")
                    val cluster = try {
                        httpGetJson("$BOOTSTRAP/api/cluster")
                    } catch (_: Exception) {
                        null
                    }
                    val role = health?.optString("role", "?") ?: "?"
                    val nodes = when {
                        cluster?.has("nodeCount") == true -> cluster.getInt("nodeCount")
                        cluster?.optJSONArray("nodes") != null -> cluster.getJSONArray("nodes").length()
                        else -> health?.optInt("nodeCount", 0) ?: 0
                    }
                    runOnUiThread {
                        detail.text = "OK role=$role nodes=$nodes\n$BOOTSTRAP"
                        btn.isEnabled = true
                    }
                } catch (e: Exception) {
                    runOnUiThread {
                        detail.text = "Unreachable: ${e.message}"
                        btn.isEnabled = true
                    }
                }
            }
        }
    }

    private fun httpGetJson(url: String): JSONObject? {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 8000
            readTimeout = 8000
            requestMethod = "GET"
        }
        return try {
            if (conn.responseCode !in 200..299) return null
            JSONObject(conn.inputStream.bufferedReader().use { it.readText() })
        } finally {
            conn.disconnect()
        }
    }

    companion object {
        const val BOOTSTRAP = "http://47.107.43.5:8443"
    }
}
