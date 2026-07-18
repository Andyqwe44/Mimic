package com.mimic.client.peer

import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.json.JSONArray
import org.json.JSONObject
import java.net.NetworkInterface
import java.security.MessageDigest
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Android peer signaling — HTTP login + WebSocket + LAN media (parity with peer_session.cpp).
 * Wire auth: passHash = hex(SHA-256(UTF-8(password))); never send plaintext password.
 */
class PeerSession(
    private val context: Context,
    private val pushToJs: (JSONObject) -> Unit,
) {
    private val tag = "MimicPeer"
    private val main = Handler(Looper.getMainLooper())
    private val http = OkHttpClient.Builder()
        .connectTimeout(12, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .build()
    private val wsClient = OkHttpClient.Builder()
        .pingInterval(20, TimeUnit.SECONDS)
        .build()

    @Volatile private var signalingHttp = ""
    @Volatile private var token = ""
    @Volatile private var user = ""
    @Volatile var deviceId: String = ""
        private set
    @Volatile private var deviceName = ""
    @Volatile private var peerDeviceId = ""
    @Volatile var online: Boolean = false
        private set
    @Volatile var role: String = "idle"
        private set
    @Volatile var transport: String = "none"
        private set
    private var ws: WebSocket? = null
    private val running = AtomicBoolean(false)
    private val lan = LanMedia(
        onJson = { handleLanJson(it) },
        onH264 = { bytes ->
            lastFrame = bytes
            push(JSONObject().put("type", "peer_frame"))
        },
    )
    @Volatile private var lastFrame: ByteArray? = null

    var onControlAction: ((JSONObject) -> JSONObject)? = null
    var onListTargets: (() -> JSONObject)? = null
    /** Controlled side: apply remote set_target (id / hwnd / display). */
    var onSetTarget: ((JSONObject) -> JSONObject)? = null
    /** Controlled side: force next encoder output to be an IDR/sync frame. */
    var onRequestKeyframe: (() -> Unit)? = null

    fun statusJson(): JSONObject = JSONObject()
        .put("ok", true)
        .put("online", online)
        .put("logged_in", online)
        .put("role", role)
        .put("transport", transport)
        .put("user", user)
        .put("deviceId", deviceId)
        .put("deviceName", deviceName)
        .put("platform", "android")
        .put("peer_proto", 2)
        .put("mediaReady", lan.ready)

    fun sendH264Packed(packed: ByteArray) {
        if (role != "controlled" || !lan.ready) return
        lan.sendH264(packed)
    }

    fun login(args: JSONObject): JSONObject {
        val base = args.optString("url", args.optString("signaling_url", DEFAULT_BOOTSTRAP)).trimEnd('/')
        val u = args.optString("user", "")
        val password = args.optString("password", "")
        val name = args.optString("deviceName", args.optString("device_name", defaultDeviceName()))
        if (u.isBlank() || password.isBlank()) {
            return err("missing user/password")
        }
        logout()
        signalingHttp = base
        deviceName = name
        deviceId = stableDeviceId()
        val lanIps = collectLanIps()
        val body = JSONObject()
            .put("user", u)
            .put("passHash", sha256Hex(password))
            .put("deviceId", deviceId)
            .put("deviceName", deviceName)
            .put("lanIps", JSONArray(lanIps))
            .put("platform", "android")
            .put("peerProto", 2)
        return try {
            val resp = httpPostJson("$base/api/login", body)
            if (!resp.optBoolean("ok", false)) {
                return if (resp.has("error")) resp else err("login failed")
            }
            token = resp.optString("token", "")
            val did = resp.optString("deviceId", "")
            if (did.isNotBlank()) deviceId = did
            user = u
            if (!openWs(base, token)) {
                return err("ws connect failed")
            }
            online = true
            role = "idle"
            Log.i(tag, "logged in user=$user device=$deviceId")
            JSONObject()
                .put("ok", true)
                .put("user", user)
                .put("deviceId", deviceId)
                .put("deviceName", deviceName)
                .put("platform", "android")
                .put("peerProto", 2)
        } catch (e: Exception) {
            Log.e(tag, "login", e)
            err(e.message ?: "network")
        }
    }

    fun register(args: JSONObject): JSONObject {
        val base = args.optString("url", args.optString("signaling_url", DEFAULT_BOOTSTRAP)).trimEnd('/')
        val u = args.optString("user", "")
        val password = args.optString("password", "")
        if (u.isBlank() || password.isBlank()) return err("missing user/password")
        return try {
            val body = JSONObject().put("user", u).put("passHash", sha256Hex(password))
            httpPostJson("$base/api/register", body)
        } catch (e: Exception) {
            err(e.message ?: "network")
        }
    }

    fun logout(): JSONObject {
        running.set(false)
        lan.stop()
        try { ws?.close(1000, "logout") } catch (_: Exception) {}
        ws = null
        token = ""
        online = false
        role = "idle"
        transport = "none"
        peerDeviceId = ""
        return JSONObject().put("ok", true)
    }

    fun invite(args: JSONObject): JSONObject {
        if (!online) return err("not logged in")
        val target = args.optString("targetDeviceId", args.optString("deviceId", ""))
        if (target.isBlank()) return err("missing targetDeviceId")
        peerDeviceId = target
        sendWs(JSONObject().put("type", "invite").put("targetDeviceId", target))
        role = "outgoing"
        return JSONObject().put("ok", true)
    }

    fun accept(args: JSONObject = JSONObject()): JSONObject {
        if (!online) return err("not logged in")
        val from = args.optString("fromDeviceId", args.optString("from", peerDeviceId))
        if (from.isBlank()) return err("missing fromDeviceId")
        peerDeviceId = from
        // Wait for session_start before LAN — matches PC peer_session.cpp
        sendWs(
            JSONObject()
                .put("type", "invite_accept")
                .put("fromDeviceId", from),
        )
        role = "ringing"
        return JSONObject().put("ok", true)
    }

    fun reject(args: JSONObject = JSONObject()): JSONObject {
        if (!online) return err("not logged in")
        val from = args.optString("fromDeviceId", args.optString("from", peerDeviceId))
        val msg = JSONObject().put("type", "invite_reject")
        if (from.isNotBlank()) msg.put("fromDeviceId", from)
        sendWs(msg)
        role = "idle"
        return JSONObject().put("ok", true)
    }

    fun hangup(): JSONObject {
        if (online) sendWs(JSONObject().put("type", "hangup"))
        lan.stop()
        role = "idle"
        transport = "none"
        return JSONObject().put("ok", true)
    }

    fun dispatch(cmd: String, args: JSONObject): JSONObject = when (cmd) {
        "peer_login" -> login(args)
        "peer_register" -> register(args)
        "peer_logout" -> logout()
        "peer_status" -> statusJson()
        "peer_invite" -> invite(args)
        "peer_accept" -> accept(args)
        "peer_reject" -> reject(args)
        "peer_hangup" -> hangup()
        "peer_request_windows" -> {
            if (!lan.ready) err("LAN not ready")
            else {
                lan.sendJson(JSONObject().put("type", "list_targets").put("peer_proto", 2))
                lan.sendJson(JSONObject().put("type", "list_windows"))
                JSONObject().put("ok", true)
            }
        }
        "peer_set_target" -> {
            if (!lan.ready) err("LAN not ready")
            else {
                val o = JSONObject().put("type", "set_target").put("peer_proto", 2)
                val id = args.optString("id", args.optString("target_id", ""))
                if (id.isNotBlank()) o.put("id", id)
                if (args.has("hwnd")) o.put("hwnd", args.optLong("hwnd"))
                val title = args.optString("title", "")
                if (title.isNotBlank()) o.put("title", title)
                lan.sendJson(o)
                JSONObject().put("ok", true)
            }
        }
        "peer_send_control" -> {
            if (!lan.ready) err("LAN not ready")
            else {
                val action = args.optJSONObject("action") ?: args
                lan.sendJson(JSONObject().put("type", "control").put("action", action))
                JSONObject().put("ok", true)
            }
        }
        "peer_set_control_mode" ->
            JSONObject().put("ok", true).put("controlMode", args.optString("mode", "human"))
        "peer_request_keyframe" -> {
            if (lan.ready) lan.sendJson(JSONObject().put("type", "need_key"))
            JSONObject().put("ok", true)
        }
        "peer_get_frame" -> {
            val fr = lastFrame
            if (fr == null || fr.size < 16) err("no frame")
            else {
                val w = java.nio.ByteBuffer.wrap(fr, 0, 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).int
                val h = java.nio.ByteBuffer.wrap(fr, 4, 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).int
                val flags = java.nio.ByteBuffer.wrap(fr, 8, 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).int
                JSONObject()
                    .put("ok", true)
                    .put("w", w).put("h", h).put("flags", flags)
                    .put("b64", android.util.Base64.encodeToString(fr, android.util.Base64.NO_WRAP))
            }
        }
        else -> err("android: unknown peer cmd '$cmd'")
    }

    private fun startLanAsControlled() {
        try {
            val port = lan.startServer(0)
            val ips = collectLanIps()
            val payload = JSONObject()
                .put("kind", "lan_offer")
                .put("port", port)
                .put("ips", JSONArray(ips))
            if (peerDeviceId.isNotBlank()) {
                sendWs(
                    JSONObject()
                        .put("type", "signal")
                        .put("toDeviceId", peerDeviceId)
                        .put("payload", payload),
                )
            }
            Log.i(tag, "LAN listen port=$port")
        } catch (e: Exception) {
            Log.e(tag, "LAN listen", e)
            push(JSONObject().put("type", "peer_error").put("error", "lan listen failed"))
        }
    }

    private fun connectLanOffer(payload: JSONObject) {
        val port = payload.optInt("port", 0)
        val ips = payload.optJSONArray("ips") ?: JSONArray()
        var ok = false
        for (i in 0 until ips.length()) {
            val ip = ips.optString(i)
            if (ip.isNotBlank() && lan.connect(ip, port)) {
                ok = true
                break
            }
        }
        if (ok) {
            transport = "lan"
            push(JSONObject().put("type", "peer_transport").put("mode", "lan"))
            if (peerDeviceId.isNotBlank()) {
                sendWs(
                    JSONObject()
                        .put("type", "signal")
                        .put("toDeviceId", peerDeviceId)
                        .put("payload", JSONObject().put("kind", "lan_ack")),
                )
            }
        } else {
            push(JSONObject().put("type", "peer_error").put("error", "LAN unreachable; WAN P2P requires ICE"))
            push(JSONObject().put("type", "peer_transport").put("mode", "none"))
        }
    }

    private fun applySessionRole(msg: JSONObject) {
        val sess = msg.optJSONObject("session")
        var ctrl = msg.optString("controllerId", "")
        var controlled = msg.optString("controlledId", "")
        if (sess != null) {
            if (ctrl.isBlank()) ctrl = sess.optString("controllerId", "")
            if (controlled.isBlank()) controlled = sess.optString("controlledId", "")
        }
        if (ctrl.isBlank() && controlled.isBlank()) return
        if (deviceId == ctrl) {
            role = "controller"
            peerDeviceId = controlled
        } else {
            role = "controlled"
            peerDeviceId = ctrl
        }
    }

    private fun handleLanJson(json: JSONObject) {
        when (json.optString("type")) {
            "list_targets", "list_windows" -> {
                // Response (has targets/windows) → forward to UI; bare request → reply.
                if (json.has("targets") || json.has("windows")) {
                    push(JSONObject().put("type", "peer_msg").put("payload", json))
                    return
                }
                val targets = onListTargets?.invoke()
                val arr = targets?.optJSONArray("targets") ?: JSONArray()
                // Normalize for controller UI: ensure hwnd + title on each entry
                val windows = JSONArray()
                for (i in 0 until arr.length()) {
                    val t = arr.optJSONObject(i) ?: continue
                    val w = JSONObject(t.toString())
                    if (!w.has("hwnd")) w.put("hwnd", t.optLong("hwnd", 0))
                    if (!w.has("title")) w.put("title", t.optString("title", t.optString("id", "")))
                    windows.put(w)
                }
                lan.sendJson(
                    JSONObject()
                        .put("type", if (json.optString("type") == "list_targets") "list_targets" else "list_windows")
                        .put("peer_proto", 2)
                        .put("targets", windows)
                        .put("windows", windows),
                )
            }
            "control" -> {
                val action = json.optJSONObject("action") ?: json
                onControlAction?.invoke(action)
            }
            "set_target" -> {
                val result = onSetTarget?.invoke(json)
                    ?: JSONObject().put("ok", false).put("error", "no set_target handler")
                val ack = JSONObject()
                    .put("type", "set_target_ack")
                    .put("ok", result.optBoolean("ok", false))
                    .put("id", json.optString("id", result.optString("id", "")))
                    .put("peer_proto", 2)
                if (result.has("error")) ack.put("error", result.optString("error"))
                lan.sendJson(ack)
            }
            "set_target_ack" -> {
                if (!json.optBoolean("ok", false)) {
                    push(
                        JSONObject()
                            .put("type", "peer_error")
                            .put("error", json.optString("error", "set_target failed")),
                    )
                }
            }
            "need_key" -> {
                onRequestKeyframe?.invoke()
                    ?: Log.w(tag, "need_key: no encoder keyframe handler")
            }
            else -> push(JSONObject().put("type", "peer_msg").put("payload", json))
        }
    }

    private fun openWs(httpBase: String, tok: String): Boolean {
        val wsUrl = httpBase
            .replace("https://", "wss://")
            .replace("http://", "ws://") + "/ws?token=$tok"
        running.set(true)
        val latchOk = AtomicBoolean(false)
        val latchDone = java.util.concurrent.CountDownLatch(1)
        val req = Request.Builder().url(wsUrl).build()
        ws = wsClient.newWebSocket(req, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                latchOk.set(true)
                latchDone.countDown()
                sendWs(
                    JSONObject()
                        .put("type", "presence")
                        .put("deviceName", deviceName)
                        .put("lanIps", JSONArray(collectLanIps()))
                        .put("platform", "android")
                        .put("peerProto", 2),
                )
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                try {
                    val msg = JSONObject(text)
                    when (msg.optString("type")) {
                        "devices" -> push(msg)
                        "invite" -> {
                            role = "ringing"
                            peerDeviceId = msg.optString("fromDeviceId", msg.optString("from", peerDeviceId))
                            push(msg)
                        }
                        "session_start", "session_state" -> {
                            applySessionRole(msg)
                            if (role == "controlled" && !lan.ready) startLanAsControlled()
                            if (lan.ready) transport = "lan"
                            push(msg)
                        }
                        "signal" -> {
                            val from = msg.optString("fromDeviceId", msg.optString("from", ""))
                            if (from.isNotBlank()) peerDeviceId = from
                            val payload = msg.optJSONObject("payload")
                            when (payload?.optString("kind")) {
                                "lan_offer" -> connectLanOffer(payload)
                                "lan_ack" -> {
                                    transport = "lan"
                                    push(JSONObject().put("type", "peer_transport").put("mode", "lan"))
                                }
                                else -> push(msg)
                            }
                        }
                        "session_end", "hangup" -> {
                            lan.stop()
                            role = "idle"
                            transport = "none"
                            push(msg)
                        }
                        "invite_rejected", "error" -> push(msg)
                        else -> push(msg)
                    }
                } catch (e: Exception) {
                    Log.w(tag, "ws msg parse", e)
                }
            }

            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                webSocket.close(1000, null)
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                online = false
                if (running.get()) {
                    push(JSONObject().put("type", "peer_offline").put("reason", "ws_closed"))
                }
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                Log.e(tag, "ws failure", t)
                latchDone.countDown()
                online = false
                if (running.get()) {
                    push(JSONObject().put("type", "peer_offline").put("reason", t.message ?: "ws_fail"))
                }
            }
        })
        latchDone.await(8, TimeUnit.SECONDS)
        return latchOk.get()
    }

    private fun sendWs(msg: JSONObject) {
        ws?.send(msg.toString())
    }

    private fun push(msg: JSONObject) {
        main.post { pushToJs(msg) }
    }

    private fun httpPostJson(url: String, body: JSONObject): JSONObject {
        val req = Request.Builder()
            .url(url)
            .post(body.toString().toRequestBody(JSON_MEDIA))
            .header("Content-Type", "application/json")
            .build()
        http.newCall(req).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            if (text.isBlank()) throw Exception("empty response HTTP ${resp.code}")
            return JSONObject(text)
        }
    }

    private fun defaultDeviceName(): String =
        "Android-${Build.MODEL ?: "device"}".take(48)

    private fun stableDeviceId(): String {
        val androidId = Settings.Secure.getString(context.contentResolver, Settings.Secure.ANDROID_ID)
            ?: Build.MODEL ?: "unknown"
        var h = 2166136261u
        for (c in androidId) {
            h = (h xor c.code.toUInt()) * 16777619u
        }
        return "adev-%08x".format(h.toInt())
    }

    private fun collectLanIps(): List<String> {
        val out = ArrayList<String>()
        try {
            val ifaces = NetworkInterface.getNetworkInterfaces() ?: return out
            for (ni in ifaces) {
                if (!ni.isUp || ni.isLoopback) continue
                for (addr in ni.inetAddresses) {
                    val host = addr.hostAddress ?: continue
                    if (host.contains(':')) continue // skip IPv6 for now
                    if (host.startsWith("127.")) continue
                    out.add(host)
                }
            }
        } catch (_: Exception) {
        }
        return out
    }

    private fun err(msg: String) = JSONObject().put("ok", false).put("error", msg)

    companion object {
        const val DEFAULT_BOOTSTRAP = "http://47.107.43.5:8443"
        private val JSON_MEDIA = "application/json; charset=utf-8".toMediaType()

        fun sha256Hex(password: String): String {
            val digest = MessageDigest.getInstance("SHA-256").digest(password.toByteArray(Charsets.UTF_8))
            val sb = StringBuilder(digest.size * 2)
            for (b in digest) sb.append("%02x".format(b))
            return sb.toString()
        }
    }
}
