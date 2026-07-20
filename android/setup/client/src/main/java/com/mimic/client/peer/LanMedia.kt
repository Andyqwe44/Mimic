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
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread

/**
 * LAN peer media framing: [type:u8][len:u32 LE][payload]
 * type 1 = H.264 (16-byte meta + Annex-B), type 2 = JSON control.
 *
 * H.264 send is async (bounded single-slot drop-old) so MediaCodec drain
 * never blocks on TCP write/flush — that was freezing first MediaProjection
 * and causing multi-second frame gaps.
 */
class LanMedia(
    private val onJson: (JSONObject) -> Unit,
    private val onH264: (ByteArray) -> Unit,
    private val onReady: (() -> Unit)? = null,
    private val onClosed: (() -> Unit)? = null,
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
    /** True while ServerSocket is bound and accept thread is alive (not yet connected). */
    val isListening: Boolean
        get() = server != null && !ready

    private val sendLock = Any()
    /** Latest pending H.264; overwritten by newer frames (drop-old). */
    @Volatile private var pendingH264: ByteArray? = null
    private val pendingJson = ArrayDeque<ByteArray>(8)
    private val writerWake = Object()
    @Volatile private var writer: Thread? = null

    private val h264Enqueued = AtomicInteger(0)
    private val h264Dropped = AtomicInteger(0)
    private val h264Sent = AtomicInteger(0)
    private val sendBlockMsMax = AtomicLong(0)

    fun startServer(preferredPort: Int = 9999): Int {
        // Idempotent: do not kill an existing listen/accept — session_state storms
        // used to rebind a new port while the peer still dialed the old one.
        if (ready) {
            Log.i(tag, "startServer skipped — already connected")
            return listenPort
        }
        if (server != null) {
            Log.i(tag, "startServer skipped — already listening port=$listenPort")
            return listenPort
        }
        val ss = ServerSocket()
        ss.reuseAddress = true
        ss.bind(InetSocketAddress(preferredPort))
        server = ss
        listenPort = ss.localPort
        running.set(true)
        thread(name = "mimic-lan-accept", isDaemon = true) {
            try {
                val s = ss.accept()
                if (ready) {
                    try { s.close() } catch (_: Exception) {}
                    Log.i(tag, "accept ignored — already connected")
                    return@thread
                }
                // Stop listening once accepted.
                try { server?.close() } catch (_: Exception) {}
                server = null
                attach(s)
            } catch (e: Exception) {
                if (running.get()) Log.w(tag, "accept failed", e)
            }
        }
        return listenPort
    }

    fun connect(host: String, port: Int): Boolean {
        if (ready) {
            Log.i(tag, "connect skipped — already ready")
            return true
        }
        return try {
            val s = Socket()
            s.tcpNoDelay = true
            s.connect(InetSocketAddress(host, port), 5000)
            // Only tear down listen AFTER outbound succeeds (reverse-dial race).
            try { server?.close() } catch (_: Exception) {}
            server = null
            attach(s)
            true
        } catch (e: Exception) {
            Log.e(tag, "connect $host:$port", e)
            false
        }
    }

    private fun attach(s: Socket) {
        try {
            s.tcpNoDelay = true
        } catch (_: Exception) {
        }
        sock = s
        out = DataOutputStream(s.getOutputStream())
        ready = true
        running.set(true)
        Log.i(tag, "LAN attached ${s.remoteSocketAddress}")
        ensureWriter()
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
                val wasReady = ready
                ready = false
                if (wasReady) {
                    try { onClosed?.invoke() } catch (ex: Exception) {
                        Log.w(tag, "onClosed", ex)
                    }
                }
            }
        }
    }

    fun sendH264(packedMetaAndAnnexB: ByteArray) {
        if (!ready || !running.get()) return
        ensureWriter()
        synchronized(sendLock) {
            if (pendingH264 != null) {
                val d = h264Dropped.incrementAndGet()
                if (d <= 3 || d % 60 == 0) {
                    Log.i(tag, "send drop-old #$d q=1 (keep latest)")
                }
            }
            pendingH264 = packedMetaAndAnnexB
            h264Enqueued.incrementAndGet()
        }
        synchronized(writerWake) { writerWake.notifyAll() }
    }

    fun sendJson(obj: JSONObject) {
        if (!ready || !running.get()) return
        ensureWriter()
        val bytes = obj.toString().toByteArray(Charsets.UTF_8)
        synchronized(sendLock) {
            // Bound control queue — drop oldest if flooded.
            while (pendingJson.size >= 8) pendingJson.removeFirst()
            pendingJson.addLast(bytes)
        }
        synchronized(writerWake) { writerWake.notifyAll() }
    }

    private fun ensureWriter() {
        val existing = writer
        if (existing != null && existing.isAlive) return
        synchronized(this) {
            val again = writer
            if (again != null && again.isAlive) return
            writer = thread(name = "mimic-lan-write", isDaemon = true) {
                writeLoop()
            }
        }
    }

    private fun writeLoop() {
        while (running.get()) {
            val type: Int
            val payload: ByteArray
            synchronized(sendLock) {
                val j = pendingJson.removeFirstOrNull()
                if (j != null) {
                    type = 2
                    payload = j
                } else {
                    val h = pendingH264
                    if (h != null) {
                        pendingH264 = null
                        type = 1
                        payload = h
                    } else {
                        type = -1
                        payload = ByteArray(0)
                    }
                }
            }
            if (type < 0) {
                synchronized(writerWake) {
                    try {
                        writerWake.wait(50)
                    } catch (_: InterruptedException) {
                        return
                    }
                }
                continue
            }
            writeNow(type, payload)
        }
    }

    private fun writeNow(type: Int, payload: ByteArray) {
        val o = out ?: return
        val t0 = System.nanoTime()
        try {
            val hdr = ByteBuffer.allocate(5).order(ByteOrder.LITTLE_ENDIAN)
            hdr.put(type.toByte())
            hdr.putInt(payload.size)
            o.write(hdr.array())
            if (payload.isNotEmpty()) o.write(payload)
            o.flush()
            if (type == 1) {
                val n = h264Sent.incrementAndGet()
                val blockMs = (System.nanoTime() - t0) / 1_000_000L
                var max = sendBlockMsMax.get()
                while (blockMs > max && !sendBlockMsMax.compareAndSet(max, blockMs)) {
                    max = sendBlockMsMax.get()
                }
                if (n <= 5 || n % 120 == 0 || blockMs >= 50) {
                    Log.i(
                        tag,
                        "H264 write #$n bytes=${payload.size} block=${blockMs}ms " +
                            "maxBlock=${sendBlockMsMax.get()}ms dropped=${h264Dropped.get()}",
                    )
                }
            }
        } catch (e: Exception) {
            Log.w(tag, "send failed", e)
            val wasReady = ready
            ready = false
            if (wasReady) {
                try { onClosed?.invoke() } catch (ex: Exception) {
                    Log.w(tag, "onClosed", ex)
                }
            }
        }
    }

    fun stop() {
        running.set(false)
        ready = false
        synchronized(writerWake) { writerWake.notifyAll() }
        try { sock?.close() } catch (_: Exception) {}
        sock = null
        out = null
        try { server?.close() } catch (_: Exception) {}
        server = null
        listenPort = 0
        synchronized(sendLock) {
            pendingH264 = null
            pendingJson.clear()
        }
        writer = null
        h264Enqueued.set(0)
        h264Dropped.set(0)
        h264Sent.set(0)
        sendBlockMsMax.set(0)
    }
}
