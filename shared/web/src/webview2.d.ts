/**
 * webview2.d.ts — Type declarations for WebView2 host objects.
 *
 * WebView2 injects window.chrome.webview into the JS runtime.
 * These types prevent silent method-name bugs (e.g. e.getAdditionalData()
 * vs e.additionalData) by making the compiler catch them.
 */

interface SharedBufferReceivedEvent {
  /** Get the shared buffer as an ArrayBuffer. */
  getBuffer(): ArrayBuffer;
  /** Additional data from PostSharedBufferToScript. WebView2 may auto-parse JSON into object. */
  readonly additionalData: string | Record<string, unknown>;
  /** Source string (empty for our push). */
  readonly source: string;
}

interface WebView2Host {
  /** Post a JSON message to the C++ host (WebMessageReceived event). */
  postMessage(json: string): void;
  /** SharedBuffer received event — fires when C++ calls PostSharedBufferToScript. */
  addEventListener(type: 'sharedbufferreceived', handler: (e: SharedBufferReceivedEvent) => void): void;
  removeEventListener(type: 'sharedbufferreceived', handler: (e: SharedBufferReceivedEvent) => void): void;
  /** WebMessage response event — fires when C++ calls PostWebMessageAsJson. */
  addEventListener(type: 'message', handler: (e: { data: any }) => void): void;
  removeEventListener(type: 'message', handler: (e: { data: any }) => void): void;
}

interface ChromeWebView {
  webview: WebView2Host;
}

declare global {
  interface Window {
    chrome?: ChromeWebView;
    /** Injected by C++ AddScriptToExecuteOnDocumentCreated before first paint. */
    __BOOT_SETTINGS__?: import('./lib/bootSettings').BootSettings;
  }
  /** Compile-time app version, injected by Vite from version.h (see vite.config.ts). */
  const __APP_VERSION__: string;
}

export {};
