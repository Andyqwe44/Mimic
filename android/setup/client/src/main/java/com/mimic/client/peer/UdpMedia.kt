package com.mimic.client.peer

import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread

data class UdpCand(val ip: String, val port: Int, val typ: String)

/**
 * STUN + UDP hole-punch + MPC2 fragments (XOR FEC 4+1, NACK, 80ms reasm).
 * Reassembled payloads match LanMedia: type 1 H.264 / type 2 JSON.
 */
class UdpMedia(
    private val onJson: (JSONObject) -> Unit,
    private val onH264: (ByteArray) -> Unit,
    private val onReady: () -> Unit,
    private val onReasmFail: ((type: Int) -> Unit)? = null,
    /** Fired when send queue drop-old overwrites an unsent frame — request IDR. */
    private val onSendDrop: (() -> Unit)? = null,
) {
    private val tag = "MimicUdp"
    private val running = AtomicBoolean(false)
    @Volatile private var sock: DatagramSocket? = null
    @Volatile private var peer: InetSocketAddress? = null
    @Volatile var ready: Boolean = false
        private set
    @Volatile var localPort: Int = 0
        private set
    private var localCands: List<UdpCand> = emptyList()
    private var remoteCands: List<UdpCand> = emptyList()
    private val msgId = AtomicInteger(1)
    private val reasm = ConcurrentHashMap<Int, Reasm>()
    private val reasmTimeouts = AtomicInteger(0)
    private val nackSent = AtomicInteger(0)
    private val fecRecovered = AtomicInteger(0)
    private val sendRing = arrayOfNulls<SendFrame>(SEND_RING)
    private var sendRingPos = 0
    private val sendLock = Any()
    @Volatile private var pendingH264: ByteArray? = null
    private val h264Dropped = AtomicInteger(0)
    private val writerWake = Object()
    @Volatile private var writer: Thread? = null

    private class Reasm(val cnt: Int, val type: Int) {
        val parts = Array(cnt) { ByteArray(0) }
        val got = BooleanArray(cnt)
        val groups = (cnt + FEC_K - 1) / FEC_K
        val parity = Array(groups) { ByteArray(0) }
        val parityGot = BooleanArray(groups)
        val startedAtMs = System.currentTimeMillis()
        @Volatile var nackSent = false
    }

    private class SendFrame(
        val frameId: Int,
        val type: Int,
        val flags: Int,
        val parts: Array<ByteArray>,
        val parity: Array<ByteArray>,
        val nackCount: IntArray,
    )

    fun start(stunHost: String, stunPort: Int = 3478): Boolean {
        stop()
        return try {
            val s = DatagramSocket(0)
            sock = s
            localPort = s.localPort
            val cands = ArrayList<UdpCand>()
            collectHostIps().forEach { ip -> cands.add(UdpCand(ip, localPort, "host")) }
            if (stunHost.isNotBlank()) {
                val srflx = stunBinding(s, stunHost, stunPort)
                if (srflx != null) {
                    cands.add(srflx)
                    Log.i(tag, "STUN srflx ${srflx.ip}:${srflx.port}")
                } else {
                    Log.w(tag, "STUN binding failed $stunHost:$stunPort")
                }
            }
            localCands = cands
            running.set(true)
            thread(name = "mimic-udp-read", isDaemon = true) { readLoop(s) }
            thread(name = "mimic-udp-reasm", isDaemon = true) { reasmTimerLoop() }
            Log.i(tag, "UDP MPC2 listen port=$localPort cands=${cands.size}")
            true
        } catch (e: Exception) {
            Log.e(tag, "start", e)
            false
        }
    }

    /** LAN host-only (no STUN). */
    fun startLan(): Boolean = start("", 0)

    fun localCands(): List<UdpCand> = localCands

    fun setRemoteCands(cands: List<UdpCand>) {
        remoteCands = cands
        thread(name = "mimic-udp-punch", isDaemon = true) {
            repeat(40) {
                if (!running.get() || ready) return@thread
                for (c in remoteCands) sendPunch(c)
                Thread.sleep(250)
            }
        }
    }

    fun sendH264(packed: ByteArray) {
        if (!ready || !running.get()) return
        ensureWriter()
        synchronized(sendLock) {
            if (pendingH264 != null) {
                val d = h264Dropped.incrementAndGet()
                if (d <= 3 || d % 60 == 0) Log.i(tag, "UDP send drop-old #$d")
                try { onSendDrop?.invoke() } catch (_: Exception) {}
            }
            pendingH264 = packed
        }
        synchronized(writerWake) { writerWake.notifyAll() }
    }

    fun sendJson(obj: JSONObject) {
        sendSync(TYPE_JSON, obj.toString().toByteArray(Charsets.UTF_8), 0)
    }

    private fun ensureWriter() {
        val existing = writer
        if (existing != null && existing.isAlive) return
        synchronized(this) {
            val again = writer
            if (again != null && again.isAlive) return
            writer = thread(name = "mimic-udp-write", isDaemon = true) {
                while (running.get()) {
                    val frame: ByteArray?
                    synchronized(sendLock) {
                        while (pendingH264 == null && running.get()) {
                            try { synchronized(writerWake) { writerWake.wait(50) } } catch (_: Exception) {}
                            if (!running.get()) break
                        }
                        frame = pendingH264
                        pendingH264 = null
                    }
                    if (frame != null) {
                        val key = frame.size >= 12 &&
                            (ByteBuffer.wrap(frame, 8, 4).order(ByteOrder.LITTLE_ENDIAN).int and 1) != 0
                        sendSync(TYPE_H264, frame, if (key) FLAG_KEY else 0)
                    }
                }
            }
        }
    }

    @Synchronized
    private fun sendSync(type: Int, payload: ByteArray, flagsIn: Int) {
        val s = sock ?: return
        val to = peer ?: return
        val mid = msgId.getAndIncrement()
        val cnt = ((payload.size + MAX_FRAG - 1) / MAX_FRAG).coerceAtLeast(1)
        var flags = flagsIn
        val useFec = (type == TYPE_H264 || type == TYPE_JSON) && cnt > 1
        if (useFec) flags = flags or FLAG_HAS_FEC
        val parts = Array(cnt) { i ->
            val off = i * MAX_FRAG
            if (off >= payload.size) ByteArray(0)
            else payload.copyOfRange(off, minOf(off + MAX_FRAG, payload.size))
        }
        val groups = (cnt + FEC_K - 1) / FEC_K
        val parity = Array(groups) { ByteArray(0) }
        for (i in 0 until cnt) {
            val chunk = parts[i]
            val fecG = i / FEC_K
            val buf = ByteBuffer.allocate(HDR + chunk.size).order(ByteOrder.LITTLE_ENDIAN)
            putHdr(buf, mid, i, cnt, type, flags, fecG)
            buf.put(chunk)
            try { s.send(DatagramPacket(buf.array(), buf.array().size, to)) } catch (e: Exception) {
                Log.w(tag, "send", e)
                return
            }
        }
        if (useFec) {
            for (g in 0 until groups) {
                var maxLen = 0
                for (i in 0 until FEC_K) {
                    val idx = g * FEC_K + i
                    if (idx >= cnt) break
                    if (parts[idx].size > maxLen) maxLen = parts[idx].size
                }
                val p = ByteArray(maxLen)
                for (i in 0 until FEC_K) {
                    val idx = g * FEC_K + i
                    if (idx >= cnt) break
                    val part = parts[idx]
                    for (b in 0 until maxLen) {
                        val v = if (b < part.size) part[b] else 0
                        p[b] = (p[b].toInt() xor v.toInt()).toByte()
                    }
                }
                parity[g] = p
                val buf = ByteBuffer.allocate(HDR + p.size).order(ByteOrder.LITTLE_ENDIAN)
                putHdr(buf, mid, 0, cnt, TYPE_FEC, flags, g)
                buf.put(p)
                try { s.send(DatagramPacket(buf.array(), buf.array().size, to)) } catch (e: Exception) {
                    Log.w(tag, "send fec", e)
                    return
                }
            }
        }
        synchronized(sendLock) {
            sendRing[sendRingPos % SEND_RING] = SendFrame(mid, type, flags, parts, parity, IntArray(cnt))
            sendRingPos++
        }
    }

    private fun putHdr(buf: ByteBuffer, frameId: Int, idx: Int, cnt: Int, type: Int, flags: Int, fecG: Int) {
        buf.putInt(MAGIC)
        buf.putInt(frameId)
        buf.putShort(idx.toShort())
        buf.putShort(cnt.toShort())
        buf.put(type.toByte())
        buf.put(flags.toByte())
        buf.putShort(fecG.toShort())
    }

    private fun sendPunch(c: UdpCand) {
        val s = sock ?: return
        try {
            val buf = ByteBuffer.allocate(18).order(ByteOrder.LITTLE_ENDIAN)
            putHdr(buf, 0, 0, 0, TYPE_PUNCH.toInt() and 0xff, 0, 0)
            buf.put('P'.code.toByte())
            buf.put('K'.code.toByte())
            s.send(DatagramPacket(buf.array(), 18, InetSocketAddress(c.ip, c.port)))
        } catch (_: Exception) {
        }
    }

    private fun lockPeer(from: InetSocketAddress) {
        if (peer == null) {
            peer = from
            ready = true
            Log.i(tag, "UDP peer locked (MPC2) $from")
            onReady()
        }
        peer = from
    }

    private fun reasmTimerLoop() {
        while (running.get()) {
            try { Thread.sleep(10) } catch (_: Exception) { break }
            purgeAndNack()
        }
    }

    private fun tryFecRecover(r: Reasm): Boolean {
        var any = false
        for (g in 0 until r.groups) {
            if (!r.parityGot[g]) continue
            var missing = -1
            var missCount = 0
            var maxLen = r.parity[g].size
            for (i in 0 until FEC_K) {
                val idx = g * FEC_K + i
                if (idx >= r.cnt) break
                if (!r.got[idx]) {
                    missing = idx
                    missCount++
                } else if (r.parts[idx].size > maxLen) maxLen = r.parts[idx].size
            }
            if (missCount != 1 || missing < 0) continue
            val recovered = r.parity[g].copyOf(maxLen)
            for (i in 0 until FEC_K) {
                val idx = g * FEC_K + i
                if (idx >= r.cnt || idx == missing) continue
                val part = r.parts[idx]
                for (b in recovered.indices) {
                    val v = if (b < part.size) part[b] else 0
                    recovered[b] = (recovered[b].toInt() xor v.toInt()).toByte()
                }
            }
            r.parts[missing] = recovered
            r.got[missing] = true
            any = true
            fecRecovered.incrementAndGet()
        }
        return any
    }

    private fun emitComplete(r: Reasm) {
        val total = r.parts.sumOf { it.size }
        val body = ByteArray(total)
        var o = 0
        for (p in r.parts) {
            System.arraycopy(p, 0, body, o, p.size)
            o += p.size
        }
        when (r.type) {
            TYPE_H264 -> onH264(body)
            TYPE_JSON -> try { onJson(JSONObject(String(body, Charsets.UTF_8))) } catch (_: Exception) {}
        }
    }

    private fun sendNack(frameId: Int, r: Reasm) {
        val s = sock ?: return
        val to = peer ?: return
        var bitmap = 0
        for (i in 0 until minOf(r.cnt, 32)) {
            if (!r.got[i]) bitmap = bitmap or (1 shl i)
        }
        if (bitmap == 0) return
        val buf = ByteBuffer.allocate(HDR + 8).order(ByteOrder.LITTLE_ENDIAN)
        putHdr(buf, frameId, 0, 1, TYPE_NACK, 0, 0)
        buf.putInt(frameId)
        buf.putInt(bitmap)
        try {
            s.send(DatagramPacket(buf.array(), buf.array().size, to))
            val n = nackSent.incrementAndGet()
            if (n <= 5 || n % 50 == 0) Log.i(tag, "UDP NACK frame=$frameId bitmap=0x${Integer.toHexString(bitmap)} #$n")
        } catch (_: Exception) {
        }
    }

    private fun retransmitNacked(frameId: Int, bitmap: Int) {
        val s = sock ?: return
        val to = peer ?: return
        val sf = synchronized(sendLock) {
            sendRing.firstOrNull { it?.frameId == frameId }
        } ?: return
        for (i in 0 until minOf(sf.parts.size, 32)) {
            if ((bitmap and (1 shl i)) == 0) continue
            if (sf.nackCount[i] >= MAX_NACK_PER_FRAG) continue
            sf.nackCount[i]++
            val chunk = sf.parts[i]
            val fecG = i / FEC_K
            val buf = ByteBuffer.allocate(HDR + chunk.size).order(ByteOrder.LITTLE_ENDIAN)
            putHdr(buf, frameId, i, sf.parts.size, sf.type, sf.flags, fecG)
            buf.put(chunk)
            try { s.send(DatagramPacket(buf.array(), buf.array().size, to)) } catch (_: Exception) {}
        }
    }

    private fun purgeAndNack() {
        val now = System.currentTimeMillis()
        val completed = ArrayList<Pair<Int, Reasm>>()
        val failed = ArrayList<Pair<Int, Int>>()
        val nacks = ArrayList<Pair<Int, Reasm>>()
        val it = reasm.entries.iterator()
        while (it.hasNext()) {
            val e = it.next()
            val r = e.value
            val age = now - r.startedAtMs
            tryFecRecover(r)
            if (r.got.all { it }) {
                completed.add(e.key to r)
                it.remove()
                continue
            }
            if (age > REASM_TIMEOUT_MS) {
                failed.add(e.key to r.type)
                it.remove()
                continue
            }
            if (!r.nackSent && age >= NACK_AFTER_MS && (r.type == TYPE_H264 || r.type == TYPE_JSON)) {
                r.nackSent = true
                nacks.add(e.key to r)
            }
        }
        for ((_, r) in completed) emitComplete(r)
        for ((fid, r) in nacks) sendNack(fid, r)
        for ((fid, type) in failed) {
            val n = reasmTimeouts.incrementAndGet()
            if (n <= 5 || n % 30 == 0) Log.w(tag, "UDP reasm timeout frame=$fid type=$type (total=$n)")
            try { onReasmFail?.invoke(type) } catch (_: Exception) {}
        }
    }

    private fun readLoop(s: DatagramSocket) {
        val buf = ByteArray(2048)
        while (running.get()) {
            try {
                val pkt = DatagramPacket(buf, buf.size)
                s.receive(pkt)
                handle(buf, pkt.length, InetSocketAddress(pkt.address, pkt.port))
            } catch (e: Exception) {
                if (running.get()) Log.w(tag, "read", e)
                break
            }
        }
        ready = false
    }

    private fun handle(data: ByteArray, n: Int, from: InetSocketAddress) {
        if (n < HDR) return
        val bb = ByteBuffer.wrap(data, 0, n).order(ByteOrder.LITTLE_ENDIAN)
        if (bb.int != MAGIC) return
        val mid = bb.int
        val idx = bb.short.toInt() and 0xffff
        val cnt = bb.short.toInt() and 0xffff
        val type = bb.get().toInt() and 0xff
        val flags = bb.get().toInt() and 0xff
        val fecG = bb.short.toInt() and 0xffff
        val payload = if (n > HDR) data.copyOfRange(HDR, n) else ByteArray(0)

        if (type == TYPE_PUNCH.toInt() and 0xff) {
            lockPeer(from)
            sendPunch(UdpCand(from.address.hostAddress ?: return, from.port, "peer"))
            return
        }
        if (type == TYPE_NACK) {
            lockPeer(from)
            if (payload.size >= 8) {
                val p = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
                retransmitNacked(p.int, p.int)
            }
            return
        }
        lockPeer(from)
        if (type == TYPE_FEC) {
            val r = reasm[mid] ?: return
            if (fecG >= r.groups) return
            r.parity[fecG] = payload
            r.parityGot[fecG] = true
            tryFecRecover(r)
            if (r.got.all { it }) {
                reasm.remove(mid)
                emitComplete(r)
            }
            return
        }
        if (cnt == 0 || idx >= cnt) return
        if (type != TYPE_H264 && type != TYPE_JSON) return
        val r = reasm.getOrPut(mid) { Reasm(cnt, type) }
        if (idx < r.parts.size) {
            r.parts[idx] = payload
            r.got[idx] = true
        }
        tryFecRecover(r)
        if (r.got.all { it }) {
            reasm.remove(mid)
            emitComplete(r)
        }
    }

    fun reasmTimeoutCount(): Int = reasmTimeouts.get()
    fun nackSentCount(): Int = nackSent.get()
    fun fecRecoveredCount(): Int = fecRecovered.get()

    fun stop() {
        running.set(false)
        ready = false
        peer = null
        synchronized(writerWake) { writerWake.notifyAll() }
        try { sock?.close() } catch (_: Exception) {}
        sock = null
        localPort = 0
        localCands = emptyList()
        remoteCands = emptyList()
        reasm.clear()
        pendingH264 = null
        writer = null
        synchronized(sendLock) {
            for (i in sendRing.indices) sendRing[i] = null
        }
    }

    companion object {
        private const val MAGIC = 0x3243504D // MPC2
        private const val HDR = 16
        private const val MAX_FRAG = 1100
        private const val FEC_K = 4
        private const val TYPE_H264 = 1
        private const val TYPE_JSON = 2
        private const val TYPE_NACK = 3
        private const val TYPE_FEC = 4
        private const val TYPE_PUNCH: Byte = 0xFF.toByte()
        private const val FLAG_KEY = 0x01
        private const val FLAG_HAS_FEC = 0x02
        private const val REASM_TIMEOUT_MS = 80L
        private const val NACK_AFTER_MS = 20L
        private const val MAX_NACK_PER_FRAG = 2
        private const val SEND_RING = 32
        private const val STUN_MAGIC = 0x2112A442

        fun candsToJson(cands: List<UdpCand>): JSONArray {
            val arr = JSONArray()
            for (c in cands) {
                arr.put(JSONObject().put("ip", c.ip).put("port", c.port).put("typ", c.typ))
            }
            return arr
        }

        fun parseCands(arr: JSONArray?): List<UdpCand> {
            if (arr == null) return emptyList()
            val out = ArrayList<UdpCand>()
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val ip = o.optString("ip")
                val port = o.optInt("port")
                if (ip.isNotBlank() && port > 0)
                    out.add(UdpCand(ip, port, o.optString("typ", "srflx")))
            }
            return out
        }

        private fun collectHostIps(): List<String> {
            val out = ArrayList<String>()
            try {
                val ifaces = NetworkInterface.getNetworkInterfaces() ?: return out
                for (ni in ifaces) {
                    if (!ni.isUp || ni.isLoopback) continue
                    for (addr in ni.inetAddresses) {
                        val h = addr.hostAddress ?: continue
                        if (h.contains(':') || h.startsWith("127.")) continue
                        out.add(h)
                    }
                }
            } catch (_: Exception) {
            }
            return out
        }

        private fun stunBinding(s: DatagramSocket, host: String, port: Int): UdpCand? {
            return try {
                s.soTimeout = 2000
                val req = ByteArray(20)
                req[0] = 0x00; req[1] = 0x01
                ByteBuffer.wrap(req, 4, 4).order(ByteOrder.BIG_ENDIAN).putInt(STUN_MAGIC)
                for (i in 8 until 20) req[i] = (System.nanoTime() shr (i * 3)).toByte()
                val dest = InetSocketAddress(InetAddress.getByName(host), port)
                s.send(DatagramPacket(req, req.size, dest))
                val resp = ByteArray(128)
                val pkt = DatagramPacket(resp, resp.size)
                s.receive(pkt)
                s.soTimeout = 0
                if (pkt.length < 28) return null
                val type = ((resp[0].toInt() and 0xff) shl 8) or (resp[1].toInt() and 0xff)
                if (type != 0x0101) return null
                var off = 20
                val len = ((resp[2].toInt() and 0xff) shl 8) or (resp[3].toInt() and 0xff)
                while (off + 4 <= pkt.length && off + 4 <= 20 + len) {
                    val at = ((resp[off].toInt() and 0xff) shl 8) or (resp[off + 1].toInt() and 0xff)
                    val al = ((resp[off + 2].toInt() and 0xff) shl 8) or (resp[off + 3].toInt() and 0xff)
                    off += 4
                    if (off + al > pkt.length) break
                    if (at == 0x0020 && al >= 8 && resp[off + 1].toInt() == 0x01) {
                        val xport = ((resp[off + 2].toInt() and 0xff) shl 8) or (resp[off + 3].toInt() and 0xff)
                        val mappedPort = xport xor ((STUN_MAGIC ushr 16) and 0xffff)
                        val ipb = ByteArray(4)
                        for (i in 0 until 4)
                            ipb[i] = (resp[off + 4 + i].toInt() xor ((STUN_MAGIC ushr (24 - 8 * i)) and 0xff)).toByte()
                        val ip = "${ipb[0].toInt() and 0xff}.${ipb[1].toInt() and 0xff}.${ipb[2].toInt() and 0xff}.${ipb[3].toInt() and 0xff}"
                        return UdpCand(ip, mappedPort, "srflx")
                    }
                    off += al
                    if (al % 4 != 0) off += 4 - (al % 4)
                }
                null
            } catch (e: Exception) {
                try { s.soTimeout = 0 } catch (_: Exception) {}
                Log.w("MimicUdp", "stun", e)
                null
            }
        }
    }
}
