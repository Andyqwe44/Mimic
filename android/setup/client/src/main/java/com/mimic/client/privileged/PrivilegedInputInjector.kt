package com.mimic.client.privileged

import android.os.SystemClock
import android.util.Log
import android.view.InputDevice
import android.view.InputEvent
import android.view.MotionEvent
import org.json.JSONObject

/**
 * Minimal InputManager injection with displayId (shell/root). Self-written reflection — not scrcpy.
 */
object PrivilegedInputInjector {
    private const val TAG = "MimicPrivInput"
    private var downTime = 0L

    fun injectNormalized(action: JSONObject, screenW: Int, screenH: Int, displayId: Int): JSONObject {
        if (displayId < 0) {
            return JSONObject().put("ok", false).put("error", "no active virtual display")
        }
        val type = action.optString("type", "")
        val x = (action.optDouble("x_norm", action.optDouble("x", 0.5)).coerceIn(0.0, 1.0) * screenW).toFloat()
        val y = (action.optDouble("y_norm", action.optDouble("y", 0.5)).coerceIn(0.0, 1.0) * screenH).toFloat()
        return when (type) {
            "mousedown" -> {
                downTime = SystemClock.uptimeMillis()
                val ok = injectTouch(MotionEvent.ACTION_DOWN, x, y, displayId)
                JSONObject().put("ok", ok).put("type", type)
                    .apply { if (!ok) put("error", "inject failed") }
            }
            "click", "tap" -> {
                downTime = SystemClock.uptimeMillis()
                val ok = injectTouch(MotionEvent.ACTION_DOWN, x, y, displayId) &&
                    injectTouch(MotionEvent.ACTION_UP, x, y, displayId)
                JSONObject().put("ok", ok).put("type", type)
                    .apply { if (!ok) put("error", "inject failed") }
            }
            "mouseup" -> {
                val ok = injectTouch(MotionEvent.ACTION_UP, x, y, displayId)
                downTime = 0L
                JSONObject().put("ok", ok).put("type", type)
            }
            "move", "drag" -> {
                if (downTime == 0L) downTime = SystemClock.uptimeMillis()
                val held = action.optBoolean("held", false)
                val act = if (held) MotionEvent.ACTION_MOVE else MotionEvent.ACTION_HOVER_MOVE
                val ok = injectTouch(act, x, y, displayId)
                JSONObject().put("ok", ok).put("type", type)
            }
            "wheel", "keydown", "keyup", "text" ->
                JSONObject().put("ok", false).put("error", "android: privileged $type not implemented yet")
            else -> JSONObject().put("ok", false).put("error", "unknown input type '$type'")
        }
    }

    private fun injectTouch(action: Int, x: Float, y: Float, displayId: Int): Boolean {
        return try {
            val now = SystemClock.uptimeMillis()
            if (action == MotionEvent.ACTION_DOWN) downTime = now
            val props = arrayOf(MotionEvent.PointerProperties().apply {
                id = 0
                toolType = MotionEvent.TOOL_TYPE_FINGER
            })
            val coords = arrayOf(MotionEvent.PointerCoords().apply {
                this.x = x
                this.y = y
                pressure = 1f
                size = 1f
            })
            val ev = MotionEvent.obtain(
                downTime, now, action, 1, props, coords,
                0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0,
            )
            setDisplayId(ev, displayId)
            val ok = injectEvent(ev)
            ev.recycle()
            ok
        } catch (e: Exception) {
            Log.w(TAG, "injectTouch", e)
            false
        }
    }

    private fun setDisplayId(event: InputEvent, displayId: Int): Boolean {
        return try {
            val m = InputEvent::class.java.getMethod("setDisplayId", Int::class.javaPrimitiveType)
            m.invoke(event, displayId)
            true
        } catch (e: Exception) {
            Log.w(TAG, "setDisplayId", e)
            false
        }
    }

    private fun injectEvent(event: InputEvent): Boolean {
        return try {
            val imClass = Class.forName("android.hardware.input.InputManager")
            val getInstance = imClass.getMethod("getInstance")
            val im = getInstance.invoke(null)
            val inject = imClass.getMethod(
                "injectInputEvent",
                InputEvent::class.java,
                Int::class.javaPrimitiveType,
            )
            // INJECT_INPUT_EVENT_MODE_ASYNC = 0
            inject.invoke(im, event, 0) as Boolean
        } catch (e: Exception) {
            Log.w(TAG, "injectInputEvent", e)
            false
        }
    }
}
