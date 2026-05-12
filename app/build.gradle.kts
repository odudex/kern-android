plugins {
    id("com.android.application")
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
}
