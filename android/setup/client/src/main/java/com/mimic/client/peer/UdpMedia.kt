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
 * STUN Binding on the media socket + UDP hole-punch + MPC1 fragments.
 * Reassembled payloads match LanMedia: type 1 H.264 / type 2 JSON.
 */
class UdpMedia(
    private val onJson: (JSONObject) -> Unit,
    private val onH264: (ByteArray) -> Unit,
    private val onReady: () -> Unit,
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

    private class Reasm(val cnt: Int, val type: Int) {
        val parts = Array(cnt) { ByteArray(0) }
        val got = BooleanArray(cnt)
    }

    fun start(stunHost: String, stunPort: Int = 3478): Boolean {
        stop()
        return try {
            val s = DatagramSocket(0)
            sock = s
            localPort = s.localPort
            val cands = ArrayList<UdpCand>()
            collectHostIps().forEach { ip ->
                cands.add(UdpCand(ip, localPort, "host"))
            }
            val srflx = stunBinding(s, stunHost, stunPort)
            if (srflx != null) {
                cands.add(srflx)
                Log.i(tag, "STUN srflx ${srflx.ip}:${srflx.port}")
            } else {
                Log.w(tag, "STUN binding failed $stunHost:$stunPort")
            }
            localCands = cands
            running.set(true)
            thread(name = "mimic-udp-read", isDaemon = true) { readLoop(s) }
            Log.i(tag, "UDP listen port=$localPort cands=${cands.size}")
            true
        } catch (e: Exception) {
            Log.e(tag, "start", e)
            false
        }
    }

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

    fun sendH264(packed: ByteArray) = send(1, packed)
    fun sendJson(obj: JSONObject) = send(2, obj.toString().toByteArray(Charsets.UTF_8))

    @Synchronized
    private fun send(type: Int, payload: ByteArray) {
        val s = sock ?: return
        val to = peer ?: return
        val mid = msgId.getAndIncrement()
        val maxFrag = 1100
        val cnt = ((payload.size + maxFrag - 1) / maxFrag).coerceAtLeast(1)
        for (i in 0 until cnt) {
            val off = i * maxFrag
            val chunk = if (off >= payload.size) ByteArray(0)
            else payload.copyOfRange(off, minOf(off + maxFrag, payload.size))
            val buf = ByteBuffer.allocate(14 + chunk.size).order(ByteOrder.LITTLE_ENDIAN)
            buf.putInt(MAGIC)
            buf.putInt(mid)
            buf.putShort(i.toShort())
            buf.putShort(cnt.toShort())
            buf.put(type.toByte())
            buf.put(0)
            buf.put(chunk)
            val pkt = DatagramPacket(buf.array(), buf.array().size, to)
            try { s.send(pkt) } catch (e: Exception) {
                Log.w(tag, "send", e)
                return
            }
        }
    }

    private fun sendPunch(c: UdpCand) {
        val s = sock ?: return
        try {
            val buf = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
            buf.putInt(MAGIC)
            buf.putInt(0)
            buf.putShort(0)
            buf.putShort(0)
            buf.put(TYPE_PUNCH)
            buf.put(0)
            buf.put('P'.code.toByte())
            buf.put('K'.code.toByte())
            val addr = InetSocketAddress(c.ip, c.port)
            s.send(DatagramPacket(buf.array(), 16, addr))
        } catch (_: Exception) {
        }
    }

    private fun lockPeer(from: InetSocketAddress) {
        if (peer == null) {
            peer = from
            ready = true
            Log.i(tag, "UDP P2P peer locked $from")
            onReady()
        }
        peer = from
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
        if (n < 14) return
        val bb = ByteBuffer.wrap(data, 0, n).order(ByteOrder.LITTLE_ENDIAN)
        if (bb.int != MAGIC) return
        val mid = bb.int
        val idx = bb.short.toInt() and 0xffff
        val cnt = bb.short.toInt() and 0xffff
        val type = bb.get().toInt() and 0xff
        bb.get() // reserved
        val payload = ByteArray(n - 14)
        if (payload.isNotEmpty()) System.arraycopy(data, 14, payload, 0, payload.size)

        if (type == 0xFF) {
            lockPeer(from)
            sendPunch(UdpCand(from.address.hostAddress ?: return, from.port, "peer"))
            return
        }
        lockPeer(from)
        if (cnt == 0 || idx >= cnt) return
        val r = reasm.getOrPut(mid) { Reasm(cnt, type) }
        if (idx < r.parts.size) {
            r.parts[idx] = payload
            r.got[idx] = true
        }
        if (r.got.all { it }) {
            val total = r.parts.sumOf { it.size }
            val body = ByteArray(total)
            var o = 0
            for (p in r.parts) {
                System.arraycopy(p, 0, body, o, p.size)
                o += p.size
            }
            reasm.remove(mid)
            when (r.type) {
                1 -> onH264(body)
                2 -> try { onJson(JSONObject(String(body, Charsets.UTF_8))) } catch (_: Exception) {}
            }
        }
    }

    fun stop() {
        running.set(false)
        ready = false
        peer = null
        try { sock?.close() } catch (_: Exception) {}
        sock = null
        localPort = 0
        localCands = emptyList()
        remoteCands = emptyList()
        reasm.clear()
    }

    companion object {
        private const val MAGIC = 0x3143504D
        private const val TYPE_PUNCH: Byte = 0xFF.toByte()
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

        /** STUN Binding using the same socket that will carry media. */
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
