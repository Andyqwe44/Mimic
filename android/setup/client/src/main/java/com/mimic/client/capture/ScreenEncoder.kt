package com.mimic.client.capture

import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface

/**
 * MediaProjection → VirtualDisplay → MediaCodec (AVC) → Annex-B packets.
 * Packet body matches PC peer_send_h264 meta (via H264AnnexB.pack).
 */
class ScreenEncoder(
    private val width: Int,
    private val height: Int,
    private val dpi: Int,
    private val bitrate: Int = 6_000_000,
    private val fps: Int = 30,
    private val onFrame: (packed: ByteArray) -> Unit,
    private val onProjectionStopped: (() -> Unit)? = null,
) {
    private val tag = "MimicEnc"
    private var codec: MediaCodec? = null
    private var surface: Surface? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var thread: HandlerThread? = null
    private var spsPps: ByteArray = ByteArray(0)
    @Volatile private var running = false
    private var projectionCallback: MediaProjection.Callback? = null
    private var projectionRef: MediaProjection? = null
    private var lastEncodeLogMs = 0L
    private var encodeCount = 0
    private var lastFrameMs = 0L
    private var gapMaxMs = 0L

    fun start(projection: MediaProjection) {
        stop()
        running = true
        projectionRef = projection
        // Scale long edge ≤1920 so Baseline Level 4.0 can encode phone full-screen.
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
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            // 1s GOP; force-sync via requestKeyframe for faster recovery.
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            // Prefer low-latency encode when the OEM exposes the key (API 30+).
            try {
                if (android.os.Build.VERSION.SDK_INT >= 30) {
                    setInteger(MediaFormat.KEY_LATENCY, 1)
                    setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
                }
            } catch (_: Exception) { /* older OEM */ }
            setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
            setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel4)
        }
        val c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        c.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        val inputSurface = c.createInputSurface()
        c.start()
        codec = c
        surface = inputSurface

        val t = HandlerThread("mimic-enc").also { it.start() }
        thread = t
        val handler = Handler(t.looper)

        // API 34+: must register callback BEFORE createVirtualDisplay or it throws.
        val cb = object : MediaProjection.Callback() {
            override fun onStop() {
                Log.w(tag, "MediaProjection.onStop")
                running = false
                handler.post {
                    releaseEncoderSurfaces()
                    onProjectionStopped?.invoke()
                }
            }
        }
        projectionCallback = cb
        projection.registerCallback(cb, handler)

        virtualDisplay = projection.createVirtualDisplay(
            "mimic-cap",
            w, h, dpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            inputSurface,
            null,
            null,
        )
        handler.post { drainLoop(w, h) }
        Log.i(tag, "encoder started ${w}x$h")
    }

    private fun drainLoop(w: Int, h: Int) {
        val c = codec ?: return
        val info = MediaCodec.BufferInfo()
        while (running) {
            val outIndex = try {
                c.dequeueOutputBuffer(info, 10_000)
            } catch (e: Exception) {
                Log.e(tag, "dequeue", e)
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
                    val mime = try { fmt.getString(MediaFormat.KEY_MIME) } catch (_: Exception) { "?" }
                    Log.i(
                        tag,
                        "outputFormat mime=$mime profile=$profile level=$level " +
                            "sps=${sps?.remaining() ?: 0}B pps=${pps?.remaining() ?: 0}B " +
                            "req=Baseline/L4",
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
                    Log.i(tag, "spsPps annexB=${spsPps.size}B ready for IDR prepend")
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
                                if (key && spsPps.isNotEmpty()) {
                                    annex = spsPps + annex
                                }
                                val ts = (info.presentationTimeUs / 1000).toInt()
                                val now = System.currentTimeMillis()
                                if (lastFrameMs > 0) {
                                    val gap = now - lastFrameMs
                                    if (gap > gapMaxMs) gapMaxMs = gap
                                }
                                lastFrameMs = now
                                encodeCount++
                                if (now - lastEncodeLogMs >= 2000 || encodeCount <= 3) {
                                    lastEncodeLogMs = now
                                    Log.i(
                                        tag,
                                        "encode #$encodeCount key=$key bytes=${annex.size} " +
                                            "gapMax=${gapMaxMs}ms",
                                    )
                                    gapMaxMs = 0
                                }
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

    /** Ask MediaCodec for the next output to be a sync/IDR frame. */
    fun requestKeyframe() {
        val c = codec ?: return
        try {
            val params = android.os.Bundle()
            params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            c.setParameters(params)
            Log.i(tag, "requested sync frame")
        } catch (e: Exception) {
            Log.w(tag, "requestKeyframe failed", e)
        }
    }

    private fun releaseEncoderSurfaces() {
        try { virtualDisplay?.release() } catch (_: Exception) {}
        virtualDisplay = null
        try { surface?.release() } catch (_: Exception) {}
        surface = null
        try {
            codec?.stop()
            codec?.release()
        } catch (_: Exception) {
        }
        codec = null
    }

    fun stop() {
        running = false
        projectionCallback?.let { cb ->
            try { projectionRef?.unregisterCallback(cb) } catch (_: Exception) {}
        }
        projectionCallback = null
        projectionRef = null
        releaseEncoderSurfaces()
        try { thread?.quitSafely() } catch (_: Exception) {}
        thread = null
    }
}
