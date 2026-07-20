package com.mimic.client

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.media.projection.MediaProjectionManager
import android.os.Bundle
import android.util.Log
import android.view.View
import android.view.ViewGroup
import android.webkit.ConsoleMessage
import android.webkit.JavascriptInterface
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.webkit.WebViewAssetLoader
import org.json.JSONObject
import java.util.concurrent.Executors

/**
 * Hosts shared/web (same React UI as Windows WebView2).
 *
 * Loads via WebViewAssetLoader (https://appassets.androidplatform.net/...) —
 * NOT file:///android_asset/ — because Chromium blocks ES modules on file://
 * which produced a blank white WebView under the ActionBar.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var webView: WebView
    private lateinit var host: AndroidHost
    private lateinit var errBanner: TextView
    private val io = Executors.newCachedThreadPool()
    private val tag = "MimicWeb"

    private val projectionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { result ->
        if (result.resultCode == Activity.RESULT_OK && result.data != null) {
            host.onProjectionResult(result.resultCode, result.data)
        } else {
            host.onProjectionResult(0, null)
        }
    }

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        supportActionBar?.hide()
        // Keep system bars chrome-matched; Web UI owns the layout (no green strip).
        WindowCompat.setDecorFitsSystemWindows(window, true)
        window.statusBarColor = Color.parseColor("#1E1E1E")
        window.navigationBarColor = Color.parseColor("#1E1E1E")

        val root = FrameLayout(this)
        webView = WebView(this).apply {
            setBackgroundColor(Color.parseColor("#1E1E1E"))
            overScrollMode = View.OVER_SCROLL_NEVER
            isVerticalScrollBarEnabled = false
            isHorizontalScrollBarEnabled = false
        }
        errBanner = TextView(this).apply {
            setBackgroundColor(Color.parseColor("#B91C1C"))
            setTextColor(Color.WHITE)
            setPadding(24, 24, 24, 24)
            textSize = 12f
            visibility = View.GONE
        }
        root.addView(
            webView,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        )
        root.addView(
            errBanner,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        )
        setContentView(root)

        host = AndroidHost(this) { msg ->
            val payload = JSONObject.quote(msg.toString())
            webView.evaluateJavascript("window.__mimicPush && window.__mimicPush($payload)", null)
        }
        host.requestProjection = {
            runOnUiThread {
                val mgr = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
                projectionLauncher.launch(mgr.createScreenCaptureIntent())
            }
        }

        val assetLoader = WebViewAssetLoader.Builder()
            .addPathHandler("/assets/", WebViewAssetLoader.AssetsPathHandler(this))
            .build()

        val s = webView.settings
        s.javaScriptEnabled = true
        s.domStorageEnabled = true
        s.allowFileAccess = true
        s.allowContentAccess = true
        s.mediaPlaybackRequiresUserGesture = false
        s.mixedContentMode = WebSettings.MIXED_CONTENT_ALWAYS_ALLOW
        s.cacheMode = WebSettings.LOAD_DEFAULT
        @Suppress("DEPRECATION")
        s.allowFileAccessFromFileURLs = true
        @Suppress("DEPRECATION")
        s.allowUniversalAccessFromFileURLs = true

        WebView.setWebContentsDebuggingEnabled(true)

        webView.webChromeClient = object : WebChromeClient() {
            override fun onConsoleMessage(consoleMessage: ConsoleMessage?): Boolean {
                val m = consoleMessage ?: return super.onConsoleMessage(consoleMessage)
                val line = "${m.messageLevel()} ${m.sourceId()}:${m.lineNumber()} ${m.message()}"
                Log.e(tag, line)
                if (m.messageLevel() == ConsoleMessage.MessageLevel.ERROR) {
                    runOnUiThread {
                        errBanner.visibility = android.view.View.VISIBLE
                        errBanner.text = (errBanner.text.toString() + "\n" + m.message()).trim()
                    }
                }
                return true
            }
        }

        webView.webViewClient = object : WebViewClient() {
            override fun shouldInterceptRequest(
                view: WebView?,
                request: WebResourceRequest?
            ): WebResourceResponse? {
                return request?.url?.let { assetLoader.shouldInterceptRequest(it) }
            }

            override fun shouldOverrideUrlLoading(
                view: WebView?,
                request: WebResourceRequest?
            ): Boolean {
                val uri = request?.url ?: return false
                val host = uri.host ?: return false
                // Keep app assets in-WebView; everything else → system browser.
                if (host == "appassets.androidplatform.net") return false
                return try {
                    startActivity(
                        Intent(Intent.ACTION_VIEW, uri).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    )
                    true
                } catch (e: Exception) {
                    Log.e(tag, "external nav failed: $uri", e)
                    true // still block in-app navigation
                }
            }

            override fun onPageStarted(view: WebView?, url: String?, favicon: android.graphics.Bitmap?) {
                injectBridgeShim()
            }

            override fun onPageFinished(view: WebView?, url: String?) {
                injectBridgeShim()
                Log.i(tag, "page finished $url")
            }

            override fun onReceivedError(
                view: WebView?,
                request: WebResourceRequest?,
                error: android.webkit.WebResourceError?
            ) {
                val msg = "WebError ${error?.errorCode}: ${error?.description} url=${request?.url}"
                Log.e(tag, msg)
                if (request?.isForMainFrame == true) {
                    runOnUiThread {
                        errBanner.visibility = android.view.View.VISIBLE
                        errBanner.text = msg
                    }
                }
            }
        }

        webView.addJavascriptInterface(JsBridge(), "MimicAndroid")
        // AssetsPathHandler maps URL path /assets/* → assets/* on disk
        // so www/index.html → https://appassets.androidplatform.net/assets/www/index.html
        webView.loadUrl("https://appassets.androidplatform.net/assets/www/index.html")
    }

    private fun injectBridgeShim() {
        val js = """
            (function(){
              if (window.__mimicBridgeReady) return;
              window.__mimicBridgeReady = true;
              window.__mimicId = 0;
              window.__mimicPending = {};
              window.__mimicResolve = function(id, result) {
                var p = window.__mimicPending[id];
                if (!p) return;
                delete window.__mimicPending[id];
                p.resolve({ result: result });
              };
              window.__mimicReject = function(id, err) {
                var p = window.__mimicPending[id];
                if (!p) return;
                delete window.__mimicPending[id];
                p.reject(new Error(err || 'native error'));
              };
              window.__mimicPush = function(raw) {
                try {
                  var msg = typeof raw === 'string' ? JSON.parse(raw) : raw;
                  if (window.__mimicOnNativePush) window.__mimicOnNativePush(msg);
                } catch (e) {}
              };
              window.Capacitor = {
                isNativePlatform: function(){ return true; },
                getPlatform: function(){ return 'android'; },
                Plugins: {
                  MimicHost: {
                    call: function(opts) {
                      return new Promise(function(resolve, reject) {
                        var id = ++window.__mimicId;
                        window.__mimicPending[id] = { resolve: resolve, reject: reject };
                        window.MimicAndroid.post(JSON.stringify({
                          id: id,
                          cmd: opts.cmd,
                          args: opts.args || {}
                        }));
                      });
                    }
                  }
                }
              };
              window.addEventListener('error', function(ev) {
                console.error('window.onerror', ev.message, ev.filename, ev.lineno);
              });
              window.addEventListener('unhandledrejection', function(ev) {
                console.error('unhandledrejection', String(ev.reason));
              });
            })();
        """.trimIndent()
        webView.evaluateJavascript(js, null)
    }

    inner class JsBridge {
        @JavascriptInterface
        fun post(msg: String) {
            io.execute {
                try {
                    val o = JSONObject(msg)
                    val id = o.getInt("id")
                    val cmd = o.getString("cmd")
                    val args = o.optJSONObject("args") ?: JSONObject()
                    val result = host.dispatch(cmd, args)
                    val resultJson = when (result) {
                        is JSONObject -> result.toString()
                        is org.json.JSONArray -> result.toString()
                        is String -> JSONObject.quote(result)
                        is Boolean -> if (result) "true" else "false"
                        is Number -> result.toString()
                        else -> JSONObject.quote(result.toString())
                    }
                    runOnUiThread {
                        webView.evaluateJavascript(
                            "window.__mimicResolve($id, $resultJson)",
                            null
                        )
                    }
                } catch (e: Exception) {
                    try {
                        val id = JSONObject(msg).optInt("id", -1)
                        if (id >= 0) {
                            runOnUiThread {
                                webView.evaluateJavascript(
                                    "window.__mimicReject($id, ${JSONObject.quote(e.message ?: "error")})",
                                    null
                                )
                            }
                        }
                    } catch (_: Exception) {
                    }
                }
            }
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (webView.canGoBack()) webView.goBack() else super.onBackPressed()
    }

    override fun onResume() {
        super.onResume()
        if (::host.isInitialized) host.onActivityResume()
    }
}
