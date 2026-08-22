// Versions live here so the app module reads as configuration rather than as a list of
// numbers. AGP 8.7 needs Gradle 8.9+, which is what the wrapper pins.
plugins {
    id("com.android.application") version "8.7.2" apply false
    id("org.jetbrains.kotlin.android") version "2.0.21" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.0.21" apply false
}
