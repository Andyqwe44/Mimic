package com.mimic.client.privileged

import android.content.ComponentName
import android.content.Context
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import com.mimic.client.BuildConfig
import com.mimic.client.IMimicFrameCallback
import com.mimic.client.IMimicPrivileged
import org.json.JSONObject
import rikka.shizuku.Shizuku
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * Binds Shizuku UserService ([PrivilegedRemoteService]) and exposes sync helpers.
 * Observes binder + permission results so preferred backend can auto-reconnect
 * after process restart (MAA-Meow-style readiness).
 */
class ShizukuConnector(private val context: Context) {
    private val tag = "MimicShizuku"
    private val mainHandler = Handler(Looper.getMainLooper())
    @Volatile private var remote: IMimicPrivileged? = null
    @Volatile private var bound = false
    private var connection: ServiceConnection? = null
    private val observing = AtomicBoolean(false)

    /** Invoked on main thread when binder appears or permission is granted. */
    @Volatile var onReady: (() -> Unit)? = null

    private val binderReceived = Shizuku.OnBinderReceivedListener {
        Log.i(tag, "binder received")
        mainHandler.post { onReady?.invoke() }
    }

    private val binderDead = Shizuku.OnBinderDeadListener {
        Log.w(tag, "binder dead")
        remote = null
        bound = false
    }

    private val permListener = Shizuku.OnRequestPermissionResultListener { _, grantResult ->
        val granted = grantResult == PackageManager.PERMISSION_GRANTED
        Log.i(tag, "permission result granted=$granted")
        if (granted) {
            mainHandler.post { onReady?.invoke() }
        }
    }

    fun startObserving() {
        if (!observing.compareAndSet(false, true)) return
        try {
            Shizuku.addBinderReceivedListenerSticky(binderReceived)
            Shizuku.addBinderDeadListener(binderDead)
            Shizuku.addRequestPermissionResultListener(permListener)
        } catch (e: Exception) {
            Log.w(tag, "startObserving", e)
            observing.set(false)
        }
    }

    fun pingAvailable(): Boolean {
        return try {
            Shizuku.pingBinder()
        } catch (_: Exception) {
            false
        }
    }

    fun permissionGranted(): Boolean {
        return try {
            Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED
        } catch (_: Exception) {
            false
        }
    }

    fun requestPermission(requestCode: Int = 1001) {
        try {
            if (!permissionGranted()) Shizuku.requestPermission(requestCode)
        } catch (e: Exception) {
            Log.w(tag, "requestPermission", e)
        }
    }

    fun isConnected(): Boolean = remote != null && bound

    fun connect(timeoutMs: Long = 8000L): JSONObject {
        if (isConnected()) {
            return JSONObject().put("ok", true).put("backend", "shizuku").put("state", "connected")
        }
        if (!pingAvailable()) {
            return JSONObject()
                .put("ok", false)
                .put("error", "android: Shizuku not running")
                .put("backend", "shizuku")
        }
        if (!permissionGranted()) {
            requestPermission()
            return JSONObject()
                .put("ok", false)
                .put("error", "android: Shizuku permission not granted")
                .put("backend", "shizuku")
                .put("need_permission", true)
        }
        val latch = CountDownLatch(1)
        val err = AtomicReference<String?>(null)
        val conn = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
                if (service == null) {
                    err.set("binder null")
                    latch.countDown()
                    return
                }
                remote = IMimicPrivileged.Stub.asInterface(service)
                bound = true
                Log.i(tag, "UserService connected")
                latch.countDown()
            }

            override fun onServiceDisconnected(name: ComponentName?) {
                Log.w(tag, "UserService disconnected")
                remote = null
                bound = false
            }
        }
        connection = conn
        return try {
            val args = Shizuku.UserServiceArgs(
                ComponentName(BuildConfig.APPLICATION_ID, PrivilegedRemoteService::class.java.name),
            )
                .daemon(false)
                .processNameSuffix("priv")
                .debuggable(BuildConfig.DEBUG)
                .version(BuildConfig.VERSION_CODE)
            Shizuku.bindUserService(args, conn)
            if (!latch.await(timeoutMs, TimeUnit.MILLISECONDS)) {
                disconnect()
                JSONObject().put("ok", false).put("error", "android: Shizuku UserService bind timeout")
            } else if (err.get() != null) {
                disconnect()
                JSONObject().put("ok", false).put("error", "android: ${err.get()}")
            } else if (remote == null) {
                disconnect()
                JSONObject().put("ok", false).put("error", "android: Shizuku UserService binder missing")
            } else {
                JSONObject().put("ok", true).put("backend", "shizuku").put("state", "connected")
            }
        } catch (e: Exception) {
            Log.e(tag, "connect failed", e)
            disconnect()
            JSONObject().put("ok", false).put("error", e.message ?: "shizuku bind failed")
        }
    }

    fun disconnect() {
        try {
            remote?.stopSession()
        } catch (_: Exception) {
        }
        remote = null
        bound = false
        val conn = connection
        connection = null
        if (conn != null) {
            try {
                val args = Shizuku.UserServiceArgs(
                    ComponentName(BuildConfig.APPLICATION_ID, PrivilegedRemoteService::class.java.name),
                )
                    .daemon(false)
                    .processNameSuffix("priv")
                    .version(BuildConfig.VERSION_CODE)
                Shizuku.unbindUserService(args, conn, true)
            } catch (e: Exception) {
                Log.w(tag, "unbind", e)
            }
        }
    }

    fun startAppSession(
        packageName: String,
        activity: String?,
        width: Int,
        height: Int,
        dpi: Int,
        onFrame: (ByteArray) -> Unit,
        onEnded: (String) -> Unit,
    ): JSONObject {
        val r = remote ?: return JSONObject().put("ok", false).put("error", "shizuku not connected")
        val cb = object : IMimicFrameCallback.Stub() {
            override fun onFrame(packed: ByteArray?) {
                if (packed != null) onFrame(packed)
            }

            override fun onSessionEnded(reason: String?) {
                onEnded(reason ?: "ended")
            }
        }
        return try {
            val json = r.startAppSession(packageName, activity, width, height, dpi, cb)
            JSONObject(json)
        } catch (e: Exception) {
            JSONObject().put("ok", false).put("error", e.message ?: "startAppSession failed")
        }
    }

    fun stopSession() {
        try { remote?.stopSession() } catch (_: Exception) {}
    }

    fun inject(action: JSONObject): JSONObject {
        val r = remote ?: return JSONObject().put("ok", false).put("error", "shizuku not connected")
        return try {
            JSONObject(r.injectJson(action.toString()))
        } catch (e: Exception) {
            JSONObject().put("ok", false).put("error", e.message ?: "inject failed")
        }
    }

    fun requestKeyframe() {
        try { remote?.requestKeyframe() } catch (_: Exception) {}
    }

    fun statusDetail(): String = when {
        !pingAvailable() -> "shizuku not running"
        !permissionGranted() -> "permission not granted"
        isConnected() -> "connected"
        else -> "available"
    }
}
