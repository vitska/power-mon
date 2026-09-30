plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "ru.vitska.powermon"
    compileSdk = 35

    defaultConfig {
        applicationId = "ru.vitska.powermon"
        // 26 rather than 21: BLE works far below this, but the runtime-permission and
        // notification-descriptor behaviour below 26 needs its own code paths, and this
        // is a tool for the person who owns the hardware, not a mass-market app.
        minSdk = 26
        targetSdk = 35
        versionCode = 24
        versionName = "0.9.13"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Signed with this machine's debug key, so the APK published on GitHub
            // installs over a development build without an uninstall -- which would
            // erase the saved boards. A personal tool; switch to a dedicated release
            // keystore before handing the app to anyone else.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.10.01")
    implementation(composeBom)

    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")

    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")

    debugImplementation("androidx.compose.ui:ui-tooling")
}
