import java.util.Properties

plugins {
    id("com.android.application")
}

// Release signing. keystore.properties (gitignored) at the repo root holds
// only non-sensitive bits (keystore path, alias). The password comes from
// the KERN_KEYSTORE_PASSWORD env var so it is never persisted to disk —
// ./release.sh prompts and forwards it via a one-shot env var. (Gradle
// runs in a daemon that detaches stdin/stdout, so prompting from inside
// the build script is not reliable.) If keystore.properties is absent,
// release builds remain unsigned and will not install on user devices.
val keystorePropertiesFile = rootProject.file("keystore.properties")
val keystoreProperties = Properties().apply {
    if (keystorePropertiesFile.exists()) {
        keystorePropertiesFile.inputStream().use(::load)
    }
}
val hasReleaseKeystore = keystorePropertiesFile.exists()

// Only resolve the password when a release task is actually being run, so
// debug builds never need the env var. Match assemble/bundle release tasks.
val isReleaseBuild = gradle.startParameter.taskNames.any { name ->
    val lower = name.lowercase()
    lower.contains("assemblerelease") || lower.contains("bundlerelease") ||
        lower.contains("installrelease") || lower == "build"
}

fun resolveKeystorePassword(): String {
    val env = System.getenv("KERN_KEYSTORE_PASSWORD")
    if (!env.isNullOrEmpty()) return env
    error(
        "Release build needs the keystore password. Use the release wrapper " +
            "which prompts and forwards it to Gradle in a one-shot env var:\n" +
            "  ./release.sh\n" +
            "(Calls ./gradlew :app:assembleRelease under the hood; extra args " +
            "are forwarded to Gradle.)"
    )
}

// Comma-separated ABI override for fast local builds. Unset -> full release
// matrix (arm64-v8a, x86_64). Examples:
//   ./gradlew :app:assembleDebug -Pkern.abi=arm64-v8a
//   echo 'kern.abi=arm64-v8a' >> ~/.gradle/gradle.properties
val kernAbis: List<String> = (findProperty("kern.abi") as String?)
    ?.split(",")
    ?.map(String::trim)
    ?.filter(String::isNotEmpty)
    ?.takeIf { it.isNotEmpty() }
    ?: listOf("arm64-v8a", "x86_64")

android {
    namespace = "com.odudex.kern"
    ndkVersion = "30.0.14904198"

    compileSdk {
        version = release(36) {
            minorApiLevel = 1
        }
    }

    defaultConfig {
        applicationId = "com.odudex.kern"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += kernAbis
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DSIM_BOARD=wave_5",
                    "-DSIM_LCD_H_RES=720",
                    "-DSIM_LCD_V_RES=1280"
                )
                cppFlags += listOf("-std=c++17")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }

    signingConfigs {
        if (hasReleaseKeystore && isReleaseBuild) {
            create("release") {
                val password = resolveKeystorePassword()
                storeFile = rootProject.file(keystoreProperties["storeFile"] as String)
                storePassword = password
                keyAlias = keystoreProperties["keyAlias"] as String
                keyPassword = password
            }
        }
    }

    buildTypes {
        getByName("release") {
            if (hasReleaseKeystore && isReleaseBuild) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }
}
