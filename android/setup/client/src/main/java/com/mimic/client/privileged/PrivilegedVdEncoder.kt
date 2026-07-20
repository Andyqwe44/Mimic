package com.mimic.client.privileged

import android.content.Context
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import com.mimic.client.capture.H264AnnexB
import org.json.JSONObject

/**
 * Independent VirtualDisplay (OWN_CONTENT_ONLY) + MediaCodec — for Shizuku/root process.
 * Not MediaProjection mirror; content only exists on this logical display.
 */
class PrivilegedVdEncoder(
    private val context: Context,
    private val width: Int,
    private val height: Int,
    private val dpi: Int,
    private val onFrame: (ByteArray) -> Unit,
    private val onEnded: (String) -> Unit,
) {
    private val tag = "MimicVdEnc"
    private var codec: MediaCodec? = null
    private var surface: Surface? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var thread: HandlerThread? = null
    private var spsPps: ByteArray = ByteArray(0)
    @Volatile private var running = false
    @Volatile var displayId: Int = -1
        private set
    private var encW = 0
    private var encH = 0

    companion object {
        private const val FLAG_PUBLIC = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC
        private const val FLAG_PRESENTATION = DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION
        private const val FLAG_OWN_CONTENT_ONLY = DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY
        /** Hidden: DisplayManager.VIRTUAL_DISPLAY_FLAG_SUPPORTS_TOUCH */
        private const val FLAG_SUPPORTS_TOUCH = 1 shl 6
        private const val FLAG_TRUSTED = 1 shl 10
        private const val FLAG_OWN_DISPLAY_GROUP = 1 shl 11
        private const val FLAG_ALWAYS_UNLOCKED = 1 shl 12
    }

    fun start(): JSONObject {
        stop()
        return try {
            running = true
            var w = width and 1.inv()
            var h = height and 1.inv()
            val maxEdge = 1920
            val longEdge = maxOf(w, h)
            if (longEdge > maxEdge) {
                val scale = maxEdge.toFloat() / longEdge
                w = (w * scale).toInt() and 1.inv()
                h = (h * scale).toInt() and 1.inv()
                if (w < 2) w = 2
                if (h < 2) h = 2
            }
            encW = w
            encH = h
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, 6_000_000)
                setInteger(MediaFormat.KEY_FRAME_RATE, 30)
                try {
                    setFloat(MediaFormat.KEY_I_FRAME_INTERVAL, 0.33f)
                } catch (_: Exception) {
                    setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
                }
                setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
                setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel4)
            }
            val c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            c.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            val inputSurface = c.createInputSurface()
            c.start()
            codec = c
            surface = inputSurface

            var flags = FLAG_PUBLIC or FLAG_PRESENTATION or FLAG_OWN_CONTENT_ONLY or FLAG_SUPPORTS_TOUCH
            if (android.os.Build.VERSION.SDK_INT >= 33) {
                flags = flags or FLAG_TRUSTED or FLAG_OWN_DISPLAY_GROUP or FLAG_ALWAYS_UNLOCKED
            }
            val dm = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
            val vd = dm.createVirtualDisplay("mimic-vd", w, h, dpi, inputSurface, flags)
                ?: return fail("createVirtualDisplay returned null")
            virtualDisplay = vd
            displayId = vd.display.displayId

            val t = HandlerThread("mimic-vd-enc").also { it.start() }
            thread = t
            Handler(t.looper).post { drainLoop(w, h) }
            Log.i(tag, "VD encoder started ${w}x$h displayId=$displayId")
            JSONObject()
                .put("ok", true)
                .put("displayId", displayId)
                .put("w", w)
                .put("h", h)
        } catch (e: Exception) {
            Log.e(tag, "start failed", e)
            stop()
            fail(e.message ?: "vd start failed")
        }
    }

    fun requestKeyframe() {
        val c = codec ?: return
        try {
            val params = android.os.Bundle()
            params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            c.setParameters(params)
        } catch (e: Exception) {
            Log.w(tag, "requestKeyframe", e)
        }
    }

    fun stop() {
        running = false
        try { virtualDisplay?.release() } catch (_: Exception) {}
        virtualDisplay = null
        displayId = -1
        try { surface?.release() } catch (_: Exception) {}
        surface = null
        try {
            codec?.stop()
            codec?.release()
        } catch (_: Exception) {
        }
        codec = null
        try { thread?.quitSafely() } catch (_: Exception) {}
        thread = null
    }

    private fun drainLoop(w: Int, h: Int) {
        val c = codec ?: return
        val info = MediaCodec.BufferInfo()
        while (running) {
            val outIndex = try {
                c.dequeueOutputBuffer(info, 10_000)
            } catch (e: Exception) {
                Log.e(tag, "dequeue", e)
                onEnded("encoder_error")
                break
            }
            when {
                outIndex == MediaCodec.INFO_TRY_AGAIN_LATER -> continue
                outIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    val fmt = c.outputFormat
                    val sps = fmt.getByteBuffer("csd-0")
                    val pps = fmt.getByteBuffer("csd-1")
                    val profile = try { fmt.getInteger(MediaFormat.KEY_PROFILE) } catch (_: Exception) { -1 }
                    val level = try { fmt.getInteger(MediaFormat.KEY_LEVEL) } catch (_: Exception) { -1 }
                    Log.i(
                        tag,
                        "outputFormat profile=$profile level=$level " +
                            "sps=${sps?.remaining() ?: 0}B pps=${pps?.remaining() ?: 0}B req=Baseline/L4",
                    )
                    val parts = ArrayList<Byte>()
                    fun addAnnex(buf: java.nio.ByteBuffer?) {
                        if (buf == null) return
                        val arr = ByteArray(buf.remaining())
                        buf.get(arr)
                        parts.add(0); parts.add(0); parts.add(0); parts.add(1)
                        for (b in arr) parts.add(b)
                    }
                    addAnnex(sps)
                    addAnnex(pps)
                    spsPps = parts.toByteArray()
                    Log.i(tag, "spsPps annexB=${spsPps.size}B")
                }
                outIndex >= 0 -> {
                    try {
                        val buf = c.getOutputBuffer(outIndex)
                        if (buf != null && info.size > 0) {
                            val raw = ByteArray(info.size)
                            buf.position(info.offset)
                            buf.limit(info.offset + info.size)
                            buf.get(raw)
                            val key = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
                            val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                            if (isConfig) {
                                spsPps = if (raw.size >= 4 && raw[0] == 0.toByte() && raw[1] == 0.toByte())
                                    raw else H264AnnexB.avccToAnnexB(raw)
                            } else {
                                var annex = if (raw.size >= 4 && raw[0] == 0.toByte() && raw[1] == 0.toByte())
                                    raw else H264AnnexB.avccToAnnexB(raw)
                                if (key && spsPps.isNotEmpty()) annex = spsPps + annex
                                val ts = (info.presentationTimeUs / 1000).toInt()
                                onFrame(H264AnnexB.pack(w, h, key, ts, annex))
                            }
                        }
                    } finally {
                        c.releaseOutputBuffer(outIndex, false)
                    }
                }
            }
        }
    }

    private fun fail(msg: String) = JSONObject().put("ok", false).put("error", msg)
}
