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
    /** Presence + reconnect on keep-alive worker — not Activity main (OEM freeze on Home). */
    private fun keepHandler(): Handler = PeerKeepAliveService.worker()
    private val http = OkHttpClient.Builder()
        .connectTimeout(12, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .build()
    private val wsClient = OkHttpClient.Builder()
        // Faster than server WS_PING (20s) so we detect stalls before roster drop.
        .pingInterval(10, TimeUnit.SECONDS)
        .build()

    @Volatile private var signalingHttp = ""
    @Volatile private var token = ""
    @Volatile private var user = ""
    @Volatile var deviceId: String = ""
        private set
    @Volatile private var deviceName = ""
    @Volatile private var peerDeviceId = ""
    /** Socket currently connected to signaling WS. */
    @Volatile var online: Boolean = false
        private set
    /** True while we hold a login token and intend to stay signed in (incl. reconnect). */
    @Volatile private var loggedIn: Boolean = false
    /** True between WS drop and successful reconnect (token still valid). */
    @Volatile private var reconnecting: Boolean = false
    @Volatile var role: String = "idle"
        private set
    @Volatile var transport: String = "none"
        private set
    private var ws: WebSocket? = null
    private val running = AtomicBoolean(false)
    private val lan = LanMedia(
        onJson = { handleLanJson(it) },
        onH264 = { bytes ->
            if (storeFramePreferKey(bytes))
                push(JSONObject().put("type", "peer_frame"))
        },
        onReady = { onMediaLinkReady("lan") },
        onClosed = {
            if (transport == "lan") {
                transport = "none"
                push(JSONObject().put("type", "peer_transport").put("mode", "none"))
                Log.w(tag, "LAN closed → transport=none")
            }
        },
    )
    private var udp: UdpMedia? = null
    @Volatile private var iceStarted = false
    @Volatile private var lastFrame: ByteArray? = null
    /** Last outbound IDR while controlled — resent when media link becomes ready. */
    @Volatile private var lastOutboundKey: ByteArray? = null
    @Volatile private var h264SentCount = 0
    @Volatile private var h264DropCount = 0
    @Volatile private var reconnectAttempt = 0
    private val reconnectRunnable = Runnable { tryReconnect() }
    private val presenceHb = object : Runnable {
        override fun run() {
            if (!running.get() || !loggedIn) return
            if (!online) {
                // Keep hb scheduled so presence resumes right after reconnect.
                if (running.get()) keepHandler().postDelayed(this, PRESENCE_MS)
                return
            }
            try {
                sendWs(
                    JSONObject()
                        .put("type", "presence")
                        .put("deviceName", deviceName)
                        .put("lanIps", JSONArray(collectLanIps()))
                        .put("platform", "android")
                        .put("peerProto", 2),
                )
                // Device list is server-pushed on join/leave; no periodic list_devices.
            } catch (e: Exception) {
                Log.w(tag, "presence hb", e)
            }
            if (running.get()) keepHandler().postDelayed(this, PRESENCE_MS)
        }
    }

    private fun storeFramePreferKey(bytes: ByteArray): Boolean {
        if (bytes.size < 12) return false
        fun flagsOf(b: ByteArray): Int =
            java.nio.ByteBuffer.wrap(b, 8, 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).int
        val newKey = (flagsOf(bytes) and 1) != 0
        val prev = lastFrame
        if (prev != null && prev.size >= 12) {
            val oldKey = (flagsOf(prev) and 1) != 0
            if (oldKey && !newKey) return false // keep unread IDR — do not notify
        }
        lastFrame = bytes
        return true
    }

    var onControlAction: ((JSONObject) -> JSONObject)? = null
    var onListTargets: (() -> JSONObject)? = null
    /** Controlled side: apply remote set_target (id / hwnd / display). */
    var onSetTarget: ((JSONObject) -> JSONObject)? = null
    /** Controlled side: force next encoder output to be an IDR/sync frame. */
    var onRequestKeyframe: (() -> Unit)? = null
    /** Session ended (local or remote) — host closes stream/control gates. */
    var onSessionEnd: (() -> Unit)? = null
    /** Controlled + media writable — host should start default Main Display capture. */
    var onControlledMediaReady: (() -> Unit)? = null

    private fun mediaReady(): Boolean = lan.ready || (udp?.ready == true)

    private fun mediaSendJson(obj: JSONObject) {
        // Control JSON (incl. mousedown/up atoms) prefers reliable LAN TCP.
        if (lan.ready) {
            lan.sendJson(obj)
            return
        }
        val u = udp
        if (u != null && u.ready) u.sendJson(obj)
    }

    fun statusJson(): JSONObject {
        // Heal missed peer_transport if socket is already up.
        if (lan.ready && transport != "lan") transport = "lan"
        else if (udp?.ready == true && transport == "none") transport = "p2p"
        return JSONObject()
            .put("ok", true)
            .put("online", online)
            .put("logged_in", loggedIn)
            .put("reconnecting", reconnecting)
            .put("role", role)
            .put("transport", transport)
            .put("user", user)
            .put("deviceId", deviceId)
            .put("deviceName", deviceName)
            .put("platform", "android")
            .put("peer_proto", 2)
            .put("mediaReady", mediaReady())
    }

    fun sendH264Packed(packed: ByteArray) {
        if (role != "controlled") return
        val isKey = packed.size >= 12 &&
            (java.nio.ByteBuffer.wrap(packed, 8, 4).order(java.nio.ByteOrder.LITTLE_ENDIAN).int and 1) != 0
        if (isKey) lastOutboundKey = packed.copyOf()
        // Prefer reliable LAN TCP when available — UDP fragments cause tearing on loss.
        if (lan.ready) {
            lan.sendH264(packed)
            noteH264Sent(isKey, viaUdp = false)
            return
        }
        val u = udp
        if (u != null && u.ready) {
            u.sendH264(packed)
            noteH264Sent(isKey, viaUdp = true)
            return
        }
        h264DropCount++
        if (h264DropCount <= 3 || h264DropCount % 60 == 0) {
            Log.w(tag, "sendH264Packed dropped — media not ready (drops=$h264DropCount key=$isKey)")
        }
    }

    private fun noteH264Sent(isKey: Boolean, viaUdp: Boolean) {
        h264SentCount++
        if (h264SentCount <= 5 || (isKey && h264SentCount % 30 == 0) || h264SentCount % 120 == 0) {
            Log.i(tag, "H264 sent #$h264SentCount key=$isKey via=${if (viaUdp) "udp" else "lan"}")
        }
    }

    /** LAN/UDP media just became writable — flush buffered IDR + kick encoder. */
    private fun onMediaLinkReady(mode: String) {
        transport = mode
        push(JSONObject().put("type", "peer_transport").put("mode", mode))
        Log.i(tag, "media link ready mode=$mode role=$role")
        if (role != "controlled") return
        val key = lastOutboundKey
        if (key != null && key.isNotEmpty()) {
            try {
                if (lan.ready) lan.sendH264(key)
                else udp?.takeIf { it.ready }?.sendH264(key)
                Log.i(tag, "resent buffered IDR (${key.size} bytes) after media ready")
            } catch (e: Exception) {
                Log.w(tag, "resent IDR failed", e)
            }
        }
        // Default Main Display capture (MediaProjection consent) — do not wait for set_target.
        main.post {
            try { onControlledMediaReady?.invoke() } catch (e: Exception) {
                Log.w(tag, "onControlledMediaReady", e)
            }
        }
        // Single keyframe request — AndroidHost also kicks once after encoder start.
        main.postDelayed({
            try { onRequestKeyframe?.invoke() } catch (_: Exception) {}
        }, 120L)
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
            loggedIn = true
            reconnecting = false
            role = "idle"
            reconnectAttempt = 0
            PeerKeepAliveService.start(context)
            keepHandler().removeCallbacks(presenceHb)
            keepHandler().postDelayed(presenceHb, PRESENCE_MS)
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
        keepHandler().removeCallbacks(reconnectRunnable)
        keepHandler().removeCallbacks(presenceHb)
        main.removeCallbacks(reconnectRunnable)
        main.removeCallbacks(presenceHb)
        reconnectAttempt = 0
        reconnecting = false
        loggedIn = false
        iceStarted = false
        try { udp?.stop() } catch (_: Exception) {}
        udp = null
        lan.stop()
        try { ws?.close(1000, "logout") } catch (_: Exception) {}
        ws = null
        token = ""
        online = false
        role = "idle"
        transport = "none"
        peerDeviceId = ""
        PeerKeepAliveService.stop(context)
        return JSONObject().put("ok", true)
    }

    fun invite(args: JSONObject): JSONObject {
        if (!online) return err(if (loggedIn) "reconnecting" else "not logged in")
        val target = args.optString("targetDeviceId", args.optString("deviceId", ""))
        if (target.isBlank()) return err("missing targetDeviceId")
        peerDeviceId = target
        sendWs(JSONObject().put("type", "invite").put("targetDeviceId", target))
        role = "outgoing"
        return JSONObject().put("ok", true)
    }

    fun accept(args: JSONObject = JSONObject()): JSONObject {
        if (!online) return err(if (loggedIn) "reconnecting" else "not logged in")
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
        if (!online) return err(if (loggedIn) "reconnecting" else "not logged in")
        val from = args.optString("fromDeviceId", args.optString("from", peerDeviceId))
        val msg = JSONObject().put("type", "invite_reject")
        if (from.isNotBlank()) msg.put("fromDeviceId", from)
        sendWs(msg)
        role = "idle"
        return JSONObject().put("ok", true)
    }

    fun hangup(): JSONObject {
        if (online) sendWs(JSONObject().put("type", "hangup"))
        iceStarted = false
        try { udp?.stop() } catch (_: Exception) {}
        udp = null
        lan.stop()
        role = "idle"
        transport = "none"
        lastOutboundKey = null
        h264SentCount = 0
        h264DropCount = 0
        return JSONObject().put("ok", true)
    }

    fun dispatch(cmd: String, args: JSONObject): JSONObject = when (cmd) {
        "peer_login" -> login(args)
        "peer_register" -> register(args)
        "peer_logout" -> logout()
        "peer_status" -> statusJson()
        "peer_list_devices" -> {
            if (!loggedIn) err("not logged in")
            else if (!online) JSONObject().put("ok", true).put("reconnecting", true)
            else {
                sendWs(JSONObject().put("type", "list_devices"))
                JSONObject().put("ok", true)
            }
        }
        "peer_invite" -> invite(args)
        "peer_accept" -> accept(args)
        "peer_reject" -> reject(args)
        "peer_hangup" -> hangup()
        "peer_request_windows" -> {
            if (!mediaReady()) err("media not ready")
            else {
                mediaSendJson(JSONObject().put("type", "list_targets").put("peer_proto", 2))
                mediaSendJson(JSONObject().put("type", "list_windows"))
                JSONObject().put("ok", true)
            }
        }
        "peer_set_target" -> {
            if (!mediaReady()) err("media not ready")
            else {
                val o = JSONObject().put("type", "set_target").put("peer_proto", 2)
                val id = args.optString("id", args.optString("target_id", ""))
                if (id.isNotBlank()) o.put("id", id)
                if (args.has("hwnd")) o.put("hwnd", args.optLong("hwnd"))
                val title = args.optString("title", "")
                if (title.isNotBlank()) o.put("title", title)
                mediaSendJson(o)
                JSONObject().put("ok", true)
            }
        }
        "peer_send_control" -> {
            if (!mediaReady()) err("media not ready")
            else {
                val action = args.optJSONObject("action") ?: args
                mediaSendJson(JSONObject().put("type", "control").put("action", action))
                JSONObject().put("ok", true)
            }
        }
        "peer_set_control_mode" ->
            JSONObject().put("ok", true).put("controlMode", args.optString("mode", "human"))
        "peer_request_keyframe" -> {
            val u = udp
            if (u != null && u.ready) u.sendJson(JSONObject().put("type", "need_key"))
            else if (lan.ready) lan.sendJson(JSONObject().put("type", "need_key"))
            JSONObject().put("ok", true)
        }
        "peer_get_frame" -> {
            val fr = lastFrame
            if (fr == null || fr.size < 16) err("no frame")
            else {
                lastFrame = null // consumed — allow next IDR to land
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
            val wasListening = lan.isListening
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
            Log.i(tag, "LAN ${if (wasListening) "re-offer" else "listen"} port=$port")
        } catch (e: Exception) {
            Log.e(tag, "LAN listen", e)
            push(JSONObject().put("type", "peer_error").put("error", "lan listen failed"))
        }
    }

    private fun connectLanOffer(payload: JSONObject) {
        if (lan.ready) {
            Log.i(tag, "lan_offer ignored — already connected")
            return
        }
        val port = payload.optInt("port", 0)
        val ips = payload.optJSONArray("ips") ?: JSONArray()
        var ok = false
        for (i in 0 until ips.length()) {
            val ip = ips.optString(i)
            if (ip.isBlank() || ip.startsWith("169.254.") || ip.startsWith("127.")) continue
            Log.i(tag, "LAN dial $ip:$port …")
            if (lan.connect(ip, port)) {
                ok = true
                break
            }
            Log.w(tag, "LAN dial failed $ip:$port")
        }
        if (ok) {
            if (peerDeviceId.isNotBlank()) {
                sendWs(
                    JSONObject()
                        .put("type", "signal")
                        .put("toDeviceId", peerDeviceId)
                        .put("payload", JSONObject().put("kind", "lan_ack")),
                )
            }
        } else {
            // Controlled still listens for reverse dial; only Controller starts ICE.
            if (role != "controller") {
                Log.w(tag, "LAN dial-out failed (still listening for reverse)")
                return
            }
            Log.w(tag, "LAN fail → ICE/STUN UDP punch")
            push(JSONObject().put("type", "peer_transport").put("mode", "ice"))
            if (peerDeviceId.isNotBlank()) {
                sendWs(
                    JSONObject()
                        .put("type", "signal")
                        .put("toDeviceId", peerDeviceId)
                        .put("payload", JSONObject().put("kind", "wan_probe").put("proto", "udp-punch").put("ver", 1)),
                )
            }
            beginIce("ice_offer")
            main.postDelayed({
                if (udp?.ready != true && transport != "lan") {
                    push(
                        JSONObject()
                            .put("type", "peer_error")
                            .put("error", "Direct P2P failed (symmetric NAT or firewall). No TURN relay in this build."),
                    )
                    push(JSONObject().put("type", "peer_transport").put("mode", "none"))
                }
            }, 15_000L)
        }
    }

    private fun stunHost(): String {
        val u = signalingHttp.removePrefix("https://").removePrefix("http://")
        return u.substringBefore('/').substringBefore(':').ifBlank { "47.107.43.5" }
    }

    private fun beginIce(kind: String) {
        if (!iceStarted) {
            iceStarted = true
            val media = UdpMedia(
                onJson = { handleLanJson(it) },
                onH264 = { bytes ->
                    if (storeFramePreferKey(bytes))
                        push(JSONObject().put("type", "peer_frame"))
                },
                onReady = {
                    onMediaLinkReady("p2p")
                    Log.i(tag, "P2P media ready (UDP hole-punch)")
                },
                onReasmFail = { type ->
                    if (type == 1) {
                        Log.i(tag, "UDP reasm fail type=1 → need_key")
                        onRequestKeyframe?.invoke()
                    }
                },
            )
            if (!media.start(stunHost(), 3478)) {
                iceStarted = false
                push(JSONObject().put("type", "peer_error").put("error", "ICE/STUN start failed"))
                return
            }
            udp = media
        }
        val cands = udp?.localCands() ?: emptyList()
        val payload = JSONObject()
            .put("kind", kind)
            .put("cands", UdpMedia.candsToJson(cands))
            .put("proto", "udp-punch")
            .put("ver", 1)
        if (peerDeviceId.isNotBlank()) {
            sendWs(
                JSONObject()
                    .put("type", "signal")
                    .put("toDeviceId", peerDeviceId)
                    .put("payload", payload),
            )
        }
        Log.i(tag, "sent $kind cands=${cands.size}")
    }

    private fun onRemoteIce(payload: JSONObject) {
        val rem = UdpMedia.parseCands(payload.optJSONArray("cands"))
        if (rem.isEmpty()) return
        if (!iceStarted) beginIce("ice_answer")
        udp?.setRemoteCands(rem)
        Log.i(tag, "ICE punch toward ${rem.size} remote cands")
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
                mediaSendJson(
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
                if (result.has("pending_consent")) {
                    ack.put("pending_consent", result.optBoolean("pending_consent"))
                }
                mediaSendJson(ack)
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
                Log.i(tag, "need_key from controller")
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
                sendWs(JSONObject().put("type", "list_devices"))
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
                            // Both roles listen+offer — reverse dial fixes PC inbound firewall.
                            // If already listening, only re-offer the same port (never rebind).
                            if ((role == "controlled" || role == "controller") && !lan.ready) {
                                startLanAsControlled()
                            }
                            if (lan.ready) {
                                transport = "lan"
                                push(JSONObject().put("type", "peer_transport").put("mode", "lan"))
                            }
                            push(msg)
                            if (role == "controlled" && lan.ready) {
                                main.post {
                                    try { onControlledMediaReady?.invoke() } catch (_: Exception) {}
                                }
                            }
                        }
                        "signal" -> {
                            val from = msg.optString("fromDeviceId", msg.optString("from", ""))
                            if (from.isNotBlank()) peerDeviceId = from
                            val payload = msg.optJSONObject("payload")
                            when (payload?.optString("kind")) {
                                "lan_offer" -> connectLanOffer(payload)
                                "lan_ack" -> {
                                    if (lan.ready) {
                                        transport = "lan"
                                        push(JSONObject().put("type", "peer_transport").put("mode", "lan"))
                                    } else {
                                        Log.w(tag, "lan_ack ignored — socket not ready")
                                    }
                                }
                                "wan_probe" -> {
                                    Log.i(tag, "wan_probe → start ICE")
                                    beginIce("ice_answer")
                                }
                                "ice_offer" -> {
                                    onRemoteIce(payload)
                                    if (role == "controlled" || role == "idle") beginIce("ice_answer")
                                }
                                "ice_answer" -> onRemoteIce(payload)
                                else -> push(msg)
                            }
                        }
                        "session_end", "hangup" -> {
                            lan.stop()
                            role = "idle"
                            transport = "none"
                            onSessionEnd?.invoke()
                            val out = if (msg.optString("type") == "hangup") {
                                JSONObject().put("type", "session_end").put("reason", "remote_hangup")
                            } else msg
                            push(out)
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
                if (ws !== webSocket) return // stale socket after reconnect
                online = false
                if (running.get() && token.isNotBlank() && loggedIn) {
                    reconnecting = true
                    push(JSONObject().put("type", "peer_reconnecting").put("reason", "ws_closed"))
                    scheduleReconnect()
                } else if (running.get()) {
                    loggedIn = false
                    reconnecting = false
                    push(JSONObject().put("type", "peer_offline").put("reason", "ws_closed"))
                }
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                if (ws !== null && ws !== webSocket) return // stale
                Log.e(tag, "ws failure", t)
                latchDone.countDown()
                online = false
                if (running.get() && token.isNotBlank() && loggedIn) {
                    reconnecting = true
                    push(JSONObject().put("type", "peer_reconnecting").put("reason", t.message ?: "ws_fail"))
                    scheduleReconnect()
                } else if (running.get()) {
                    loggedIn = false
                    reconnecting = false
                    push(JSONObject().put("type", "peer_offline").put("reason", t.message ?: "ws_fail"))
                }
            }
        })
        latchDone.await(8, TimeUnit.SECONDS)
        return latchOk.get()
    }

    private fun scheduleReconnect() {
        if (!running.get() || token.isBlank() || signalingHttp.isBlank() || !loggedIn) return
        keepHandler().removeCallbacks(reconnectRunnable)
        // First retry almost immediately — Home→freeze often drops WS within seconds.
        val delay = if (reconnectAttempt == 0) 400L
        else (1000L * (1 shl (reconnectAttempt - 1).coerceAtMost(5))).coerceAtMost(30_000L)
        reconnectAttempt++
        Log.i(tag, "ws reconnect in ${delay}ms attempt=$reconnectAttempt")
        keepHandler().postDelayed(reconnectRunnable, delay)
    }

    private fun tryReconnect() {
        if (!running.get() || token.isBlank() || signalingHttp.isBlank() || !loggedIn) return
        Log.i(tag, "ws reconnect attempt=$reconnectAttempt")
        try {
            try { ws?.cancel() } catch (_: Exception) {}
            ws = null
            if (openWs(signalingHttp, token)) {
                online = true
                reconnecting = false
                reconnectAttempt = 0
                PeerKeepAliveService.start(context)
                // Resume presence heartbeat after reconnect.
                keepHandler().removeCallbacks(presenceHb)
                keepHandler().postDelayed(presenceHb, PRESENCE_MS)
                push(JSONObject().put("type", "peer_online").put("reconnected", true))
                Log.i(tag, "ws reconnected")
            } else {
                scheduleReconnect()
            }
        } catch (e: Exception) {
            Log.e(tag, "reconnect", e)
            scheduleReconnect()
        }
    }

    /**
     * Activity onResume — kick reconnect / presence if WS died while backgrounded.
     * Safe to call often; no-op when already online.
     */
    fun ensureOnline() {
        if (!loggedIn || token.isBlank() || signalingHttp.isBlank()) return
        PeerKeepAliveService.start(context)
        if (online && ws != null) {
            // Refresh roster for peers that missed pushes while we were frozen.
            try { sendWs(JSONObject().put("type", "list_devices")) } catch (_: Exception) {}
            return
        }
        if (!reconnecting) {
            reconnecting = true
            push(JSONObject().put("type", "peer_reconnecting").put("reason", "resume"))
        }
        reconnectAttempt = 0
        keepHandler().removeCallbacks(reconnectRunnable)
        keepHandler().post { tryReconnect() }
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
        val priv = ArrayList<String>()
        val other = ArrayList<String>()
        try {
            val ifaces = NetworkInterface.getNetworkInterfaces() ?: return priv
            for (ni in ifaces) {
                if (!ni.isUp || ni.isLoopback) continue
                for (addr in ni.inetAddresses) {
                    val host = addr.hostAddress ?: continue
                    if (host.contains(':')) continue // skip IPv6 for now
                    if (host.startsWith("127.") || host.startsWith("169.254.")) continue
                    val isPriv = host.startsWith("10.") || host.startsWith("192.168.") ||
                        (host.startsWith("172.") && run {
                            val second = host.substringAfter('.').substringBefore('.').toIntOrNull() ?: -1
                            second in 16..31
                        })
                    if (isPriv) priv.add(host) else other.add(host)
                }
            }
        } catch (_: Exception) {
        }
        return priv + other
    }

    private fun err(msg: String) = JSONObject().put("ok", false).put("error", msg)

    companion object {
        const val DEFAULT_BOOTSTRAP = "http://47.107.43.5:8443"
        /** Presence interval — shorter than server WS_PING so NAT/OEM freezes recover faster. */
        private const val PRESENCE_MS = 8_000L
        private val JSON_MEDIA = "application/json; charset=utf-8".toMediaType()

        fun sha256Hex(password: String): String {
            val digest = MessageDigest.getInstance("SHA-256").digest(password.toByteArray(Charsets.UTF_8))
            val sb = StringBuilder(digest.size * 2)
            for (b in digest) sb.append("%02x".format(b))
            return sb.toString()
        }
    }
}
