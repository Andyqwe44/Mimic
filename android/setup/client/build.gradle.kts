plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.mimic.client"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.mimic.client"
        minSdk = 26
        targetSdk = 34
        versionCode = 13
        versionName = "0.1.12"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures {
        viewBinding = true
        buildConfig = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
    // Serve assets via https://appassets.androidplatform.net so Vite ES modules work
    // (file:///android_asset/ cannot load <script type="module"> → white screen)
    implementation("androidx.webkit:webkit:1.11.0")
    // Peer signaling WebSocket + HTTP (parity with pc/client peer_session)
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
}
