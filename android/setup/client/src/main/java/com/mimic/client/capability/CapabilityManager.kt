package com.mimic.client.capability

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.mimic.client.privileged.ShizukuConnector
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Explicit privilege backend selector (铁律 5 — no silent fallback).
 * Preferred backend is persisted; restore runs **off the main thread** so
 * Shizuku UserService bind cannot freeze Activity / WebView startup.
 */
class CapabilityManager(private val context: Context) {
    @Volatile var active: CapabilityBackend = CapabilityBackend.NORMAL
        private set

    val shizuku = ShizukuConnector(context)

    private val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val restoreExec = Executors.newSingleThreadExecutor { r ->
        Thread(r, "mimic-cap-restore").apply { isDaemon = true }
    }
    private val mainHandler = Handler(Looper.getMainLooper())
    private val restoring = AtomicBoolean(false)
    private val tag = "MimicCap"

    private val statuses = mutableMapOf(
        CapabilityBackend.NORMAL to BackendStatus(CapabilityBackend.NORMAL, BackendState.Connected, "default"),
        CapabilityBackend.SHIZUKU to BackendStatus(CapabilityBackend.SHIZUKU, BackendState.Unavailable, "probing"),
        CapabilityBackend.ROOT to BackendStatus(CapabilityBackend.ROOT, BackendState.Unavailable, "not implemented yet"),
    )

    /** Optional: push status to Web UI when restore finishes. */
    @Volatile var onStatusChanged: (() -> Unit)? = null

    init {
        shizuku.onReady = {
            // Never bind on the main thread — schedule async restore only.
            scheduleRestorePreferred("binder/permission ready")
        }
        shizuku.startObserving()
        refreshShizukuStatus()
        // Cold start: if user preferred Shizuku, reconnect in background (non-blocking).
        scheduleRestorePreferred("init")
    }

    fun preferredBackend(): CapabilityBackend {
        val id = prefs.getString(KEY_BACKEND, CapabilityBackend.NORMAL.id)
            ?: CapabilityBackend.NORMAL.id
        return CapabilityBackend.fromId(id) ?: CapabilityBackend.NORMAL
    }

    private fun persistPreferred(backend: CapabilityBackend) {
        prefs.edit().putString(KEY_BACKEND, backend.id).apply()
    }

    fun scheduleRestorePreferred(reason: String) {
        if (preferredBackend() != CapabilityBackend.SHIZUKU) return
        if (active == CapabilityBackend.SHIZUKU && shizuku.isConnected()) return
        if (!restoring.compareAndSet(false, true)) {
            Log.i(tag, "restore already in flight ($reason)")
            return
        }
        Log.i(tag, "schedule restore ($reason)")
        restoreExec.execute {
            try {
                restorePreferredBlocking()
            } finally {
                restoring.set(false)
                mainHandler.post { onStatusChanged?.invoke() }
            }
        }
    }

    /** Runs only on restoreExec — may block up to connect timeout. */
    private fun restorePreferredBlocking() {
        if (preferredBackend() != CapabilityBackend.SHIZUKU) return
        if (active == CapabilityBackend.SHIZUKU && shizuku.isConnected()) return
        refreshShizukuStatus()
        if (!shizuku.pingAvailable()) {
            Log.i(tag, "restore skip: shizuku not running")
            return
        }
        if (!shizuku.permissionGranted()) {
            Log.i(tag, "restore skip: permission not granted (preference kept)")
            return
        }
        mark(CapabilityBackend.SHIZUKU, BackendState.Connecting, "restoring")
        val conn = shizuku.connect(timeoutMs = 6_000L)
        if (conn.optBoolean("ok", false)) {
            active = CapabilityBackend.SHIZUKU
            persistPreferred(CapabilityBackend.SHIZUKU)
            refreshShizukuStatus()
            Log.i(tag, "restore ok — backend=shizuku")
        } else {
            refreshShizukuStatus()
            Log.w(tag, "restore failed: ${conn.optString("error")}")
        }
    }

    fun refreshShizukuStatus() {
        val st = when {
            restoring.get() && preferredBackend() == CapabilityBackend.SHIZUKU ->
                BackendStatus(CapabilityBackend.SHIZUKU, BackendState.Connecting, "restoring")
            !shizuku.pingAvailable() ->
                BackendStatus(CapabilityBackend.SHIZUKU, BackendState.Unavailable, "shizuku not running")
            !shizuku.permissionGranted() ->
                BackendStatus(CapabilityBackend.SHIZUKU, BackendState.Available, "permission not granted")
            shizuku.isConnected() ->
                BackendStatus(CapabilityBackend.SHIZUKU, BackendState.Connected, "connected")
            else ->
                BackendStatus(CapabilityBackend.SHIZUKU, BackendState.Available, "available")
        }
        statuses[CapabilityBackend.SHIZUKU] = st
    }

    fun statusJson(): JSONObject {
        refreshShizukuStatus()
        val available = JSONArray()
        for ((backend, st) in statuses) {
            if (st.state == BackendState.Connected || st.state == BackendState.Available ||
                st.state == BackendState.Connecting
            ) {
                available.put(backend.id)
            }
        }
        fun one(b: CapabilityBackend): JSONObject {
            val st = statuses[b]!!
            return JSONObject()
                .put("available", st.state != BackendState.Unavailable)
                .put("granted", st.state == BackendState.Connected)
                .put("state", st.state.name.lowercase())
                .put("detail", st.detail)
        }
        return JSONObject()
            .put("ok", true)
            .put("backend", active.id)
            .put("preferred", preferredBackend().id)
            .put("restoring", restoring.get())
            .put("available", available)
            .put("shizuku", one(CapabilityBackend.SHIZUKU))
            .put("root", one(CapabilityBackend.ROOT))
            .put("normal", one(CapabilityBackend.NORMAL))
    }

    fun setBackend(id: String): JSONObject {
        val backend = CapabilityBackend.fromId(id)
            ?: return JSONObject().put("ok", false).put("error", "unknown backend '$id'")
        refreshShizukuStatus()
        val st = statuses[backend]!!
        if (backend == CapabilityBackend.ROOT) {
            return JSONObject()
                .put("ok", false)
                .put("error", "android: backend 'root' not implemented yet")
                .put("backend", active.id)
        }
        if (backend == CapabilityBackend.SHIZUKU) {
            // Remember intent immediately so cold start restores even if this bind fails.
            persistPreferred(CapabilityBackend.SHIZUKU)
            if (st.state == BackendState.Unavailable) {
                return JSONObject()
                    .put("ok", false)
                    .put("error", "android: backend 'shizuku' unavailable (${st.detail})")
                    .put("backend", active.id)
            }
            val conn = shizuku.connect()
            if (!conn.optBoolean("ok", false)) {
                refreshShizukuStatus()
                return conn.put("backend", active.id)
            }
            refreshShizukuStatus()
            active = CapabilityBackend.SHIZUKU
            return JSONObject().put("ok", true).put("backend", active.id)
        }
        // NORMAL
        if (active == CapabilityBackend.SHIZUKU) {
            shizuku.stopSession()
            shizuku.disconnect()
        }
        active = CapabilityBackend.NORMAL
        persistPreferred(CapabilityBackend.NORMAL)
        return JSONObject().put("ok", true).put("backend", active.id)
    }

    fun ensureShizukuConnected(): JSONObject {
        refreshShizukuStatus()
        if (shizuku.isConnected()) {
            active = CapabilityBackend.SHIZUKU
            persistPreferred(CapabilityBackend.SHIZUKU)
            return JSONObject().put("ok", true).put("backend", "shizuku")
        }
        persistPreferred(CapabilityBackend.SHIZUKU)
        val conn = shizuku.connect()
        if (conn.optBoolean("ok", false)) {
            active = CapabilityBackend.SHIZUKU
            refreshShizukuStatus()
        }
        return conn
    }

    fun mark(backend: CapabilityBackend, state: BackendState, detail: String = "") {
        statuses[backend] = BackendStatus(backend, state, detail)
    }

    fun canVirtualDisplay(): Boolean =
        active == CapabilityBackend.SHIZUKU || active == CapabilityBackend.ROOT ||
            (shizuku.pingAvailable() && shizuku.permissionGranted())

    companion object {
        private const val PREFS = "mimic_capability"
        private const val KEY_BACKEND = "preferred_backend"
    }
}
