package com.mimic.client.peer

import android.util.Log
import org.json.JSONObject
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * LAN peer media framing: [type:u8][len:u32 LE][payload]
 * type 1 = H.264 (16-byte meta + Annex-B), type 2 = JSON control.
 */
class LanMedia(
    private val onJson: (JSONObject) -> Unit,
    private val onH264: (ByteArray) -> Unit,
    private val onReady: (() -> Unit)? = null,
) {
    private val tag = "MimicLan"
    private val running = AtomicBoolean(false)
    @Volatile private var sock: Socket? = null
    @Volatile private var out: DataOutputStream? = null
    @Volatile private var server: ServerSocket? = null
    @Volatile var ready: Boolean = false
        private set
    @Volatile var listenPort: Int = 0
        private set

    fun startServer(preferredPort: Int = 9999): Int {
        stop()
        val ss = ServerSocket()
        ss.reuseAddress = true
        ss.bind(InetSocketAddress(preferredPort))
        server = ss
        listenPort = ss.localPort
        running.set(true)
        thread(name = "mimic-lan-accept", isDaemon = true) {
            try {
                val s = ss.accept()
                attach(s)
            } catch (e: Exception) {
                if (running.get()) Log.w(tag, "accept failed", e)
            }
        }
        return listenPort
    }

    fun connect(host: String, port: Int): Boolean {
        stop()
        return try {
            val s = Socket()
            s.connect(InetSocketAddress(host, port), 5000)
            attach(s)
            true
        } catch (e: Exception) {
            Log.e(tag, "connect $host:$port", e)
            false
        }
    }

    private fun attach(s: Socket) {
        sock = s
        out = DataOutputStream(s.getOutputStream())
        ready = true
        running.set(true)
        Log.i(tag, "LAN attached ${s.remoteSocketAddress}")
        try { onReady?.invoke() } catch (e: Exception) { Log.w(tag, "onReady", e) }
        thread(name = "mimic-lan-read", isDaemon = true) {
            try {
                val inp = DataInputStream(s.getInputStream())
                while (running.get()) {
                    val type = inp.readUnsignedByte()
                    val lenBytes = ByteArray(4)
                    inp.readFully(lenBytes)
                    val len = ByteBuffer.wrap(lenBytes).order(ByteOrder.LITTLE_ENDIAN).int
                    if (len < 0 || len > 16 * 1024 * 1024) break
                    val payload = ByteArray(len)
                    if (len > 0) inp.readFully(payload)
                    when (type) {
                        1 -> onH264(payload)
                        2 -> {
                            try {
                                onJson(JSONObject(String(payload, Charsets.UTF_8)))
                            } catch (_: Exception) {
                            }
                        }
                    }
                }
            } catch (e: Exception) {
                if (running.get()) Log.w(tag, "read end", e)
            } finally {
                ready = false
            }
        }
    }

    fun sendH264(packedMetaAndAnnexB: ByteArray) {
        send(1, packedMetaAndAnnexB)
    }

    fun sendJson(obj: JSONObject) {
        send(2, obj.toString().toByteArray(Charsets.UTF_8))
    }

    @Synchronized
    private fun send(type: Int, payload: ByteArray) {
        val o = out ?: return
        try {
            val hdr = ByteBuffer.allocate(5).order(ByteOrder.LITTLE_ENDIAN)
            hdr.put(type.toByte())
            hdr.putInt(payload.size)
            o.write(hdr.array())
            if (payload.isNotEmpty()) o.write(payload)
            o.flush()
        } catch (e: Exception) {
            Log.w(tag, "send failed", e)
            ready = false
        }
    }

    fun stop() {
        running.set(false)
        ready = false
        try { sock?.close() } catch (_: Exception) {}
        sock = null
        out = null
        try { server?.close() } catch (_: Exception) {}
        server = null
        listenPort = 0
    }
}
