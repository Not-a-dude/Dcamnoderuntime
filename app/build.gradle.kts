plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.dcamnoderuntime"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.dcamnoderuntime"
        minSdk = 31
        targetSdk = 37
        versionCode = 2
        versionName = "0.2"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            abiFilters.add("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                targets.add("dcam_node_loader")
            }
        }
    }

    signingConfigs {
        create("dcamConfig") {
            val storeFilePath = providers.gradleProperty("DCAM_STORE_FILE").orNull
            val storePasswordProp = providers.gradleProperty("DCAM_STORE_PASSWORD").orNull
            val keyAliasProp = providers.gradleProperty("DCAM_KEY_ALIAS").orNull
            val keyPasswordProp = providers.gradleProperty("DCAM_KEY_PASSWORD").orNull

            if (!storeFilePath.isNullOrEmpty()) {
                storeFile = file(storeFilePath)
                storePassword = storePasswordProp
                keyAlias = keyAliasProp
                keyPassword = keyPasswordProp
            }
        }
    }

    buildTypes {
        debug {
            val dcam = signingConfigs.getByName("dcamConfig")
            if (dcam.storeFile != null) {
                signingConfig = dcam
            }
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            val dcam = signingConfigs.getByName("dcamConfig")
            if (dcam.storeFile != null) {
                signingConfig = dcam
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "src/main/keepRules/rules.keep",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        aidl = true
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
    ndkVersion = "30.0.16248370"
}

dependencies {
    implementation(libs.androidx.core.ktx)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
}