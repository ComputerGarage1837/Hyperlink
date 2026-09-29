plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// One version for the whole project, read from the repository's VERSION file.
val appVersion: String = rootProject.file("../VERSION").readText().trim()

fun versionCodeFrom(v: String): Int {
    val p = v.split(".").map { part -> part.takeWhile { it.isDigit() }.toIntOrNull() ?: 0 }
    return p.getOrElse(0) { 0 } * 10000 + p.getOrElse(1) { 0 } * 100 + p.getOrElse(2) { 0 }
}

android {
    namespace = "com.hyperlink.app"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.computergarage.hyperlink"
        minSdk = 26
        targetSdk = 35
        versionCode = versionCodeFrom(appVersion)
        versionName = appVersion
        buildConfigField("String", "GITHUB_REPO", "\"ComputerGarage1837/Hyperlink\"")
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
            }
        }
    }

    // Every build is signed with the same key so updates install over each other.
    // The key is in the repo on purpose (personal project); swap in your own via the
    // HYPERLINK_KEYSTORE* environment variables if you ever want to.
    signingConfigs {
        create("release") {
            storeFile = file(System.getenv("HYPERLINK_KEYSTORE") ?: "../hyperlink-release.jks")
            storePassword = System.getenv("HYPERLINK_KEYSTORE_PASSWORD") ?: "hyperlink"
            keyAlias = System.getenv("HYPERLINK_KEY_ALIAS") ?: "hyperlink"
            keyPassword = System.getenv("HYPERLINK_KEY_PASSWORD") ?: "hyperlink"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
        }
        debug {
            signingConfig = signingConfigs.getByName("release")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        buildConfig = true
        viewBinding = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.6")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
