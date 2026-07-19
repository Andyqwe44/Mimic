package com.mimic.client.capability

import android.content.Context
import com.mimic.client.privileged.ShizukuConnector
import org.json.JSONArray
import org.json.JSONObject

/**
 * Explicit privilege backend selector (铁律 5 — no silent fallback).
 * normal always available; shizuku when Shizuku app is present + permission.
 *
 * Preferred backend is persisted (SharedPreferences) so a prior Shizuku choice
 * survives process death — MAA-Meow-style: remember + auto-reconnect on ready.
 */
class CapabilityManager(private val context: Context) {
    @Volatile var active: CapabilityBackend = CapabilityBackend.NORMAL
        private set

    val shizuku = ShizukuConnector(context)

    private val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private val statuses = mutableMapOf(
        CapabilityBackend.NORMAL to BackendStatus(CapabilityBackend.NORMAL, BackendState.Connected, "default"),
        CapabilityBackend.SHIZUKU to BackendStatus(CapabilityBackend.SHIZUKU, BackendState.Unavailable, "probing"),
        CapabilityBackend.ROOT to BackendStatus(CapabilityBackend.ROOT, BackendState.Unavailable, "not implemented yet"),
    )

    init {
        shizuku.onReady = {
            // Shizuku binder / permission became ready — restore preferred if needed.
            maybeRestorePreferred()
            refreshShizukuStatus()
        }
        shizuku.startObserving()
        refreshShizukuStatus()
        maybeRestorePreferred()
    }

    fun preferredBackend(): CapabilityBackend {
        val id = prefs.getString(KEY_BACKEND, CapabilityBackend.NORMAL.id)
            ?: CapabilityBackend.NORMAL.id
        return CapabilityBackend.fromId(id) ?: CapabilityBackend.NORMAL
    }

    private fun persistPreferred(backend: CapabilityBackend) {
        prefs.edit().putString(KEY_BACKEND, backend.id).apply()
    }

    /** Cold-start / binder-up: if user preferred Shizuku, reconnect UserService. */
    fun maybeRestorePreferred() {
        if (preferredBackend() != CapabilityBackend.SHIZUKU) return
        if (active == CapabilityBackend.SHIZUKU && shizuku.isConnected()) return
        refreshShizukuStatus()
        val st = statuses[CapabilityBackend.SHIZUKU]!!
        if (st.state == BackendState.Unavailable) return
        if (!shizuku.permissionGranted()) {
            // Permission may still be granted at system level after app restart;
            // if not, leave Available so UI can prompt — do not clear preference.
            return
        }
        val conn = shizuku.connect()
        if (conn.optBoolean("ok", false)) {
            active = CapabilityBackend.SHIZUKU
            refreshShizukuStatus()
        }
    }

    fun refreshShizukuStatus() {
        val st = when {
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
            if (st.state == BackendState.Connected || st.state == BackendState.Available) {
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
            if (st.state == BackendState.Unavailable) {
                return JSONObject()
                    .put("ok", false)
                    .put("error", "android: backend 'shizuku' unavailable (${st.detail})")
                    .put("backend", active.id)
            }
            val conn = shizuku.connect()
            if (!conn.optBoolean("ok", false)) {
                refreshShizukuStatus()
                // Still remember preference so next binder/permission grant auto-restores.
                if (conn.optBoolean("need_permission", false)) {
                    persistPreferred(CapabilityBackend.SHIZUKU)
                }
                return conn.put("backend", active.id)
            }
            refreshShizukuStatus()
            active = CapabilityBackend.SHIZUKU
            persistPreferred(CapabilityBackend.SHIZUKU)
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
        val conn = shizuku.connect()
        if (conn.optBoolean("ok", false)) {
            active = CapabilityBackend.SHIZUKU
            persistPreferred(CapabilityBackend.SHIZUKU)
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
