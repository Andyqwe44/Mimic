# MimicHost Capacitor plugin

Copy `MimicHost/MimicHostPlugin.kt` into the generated Android app after `npx cap add android`:

```
android/app/src/main/java/com/mimic/client/plugins/MimicHostPlugin.kt
```

Register in `MainActivity.java` / `.kt`:

```kotlin
import com.mimic.client.plugins.MimicHostPlugin

class MainActivity : BridgeActivity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    registerPlugin(MimicHostPlugin::class.java)
    super.onCreate(savedInstanceState)
  }
}
```

Add a FileProvider in `AndroidManifest.xml` for APK install (`${applicationId}.fileprovider`) pointing at `cache/updates/`.

JS side: `shared/web/src/lib/bridge.ts` routes `hostCall` → `Capacitor.Plugins.MimicHost.call`.
