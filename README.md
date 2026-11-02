# Black for Android
An unofficial native Android port of the original Xbox version of *Black*, with touch controls, gyroscope aiming, controller support and extended graphics options.

The game's x86 code is statically recompiled ahead of time to C++ with [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), then built for both 64-bit and 32-bit ARM Android devices. Drawing goes through Vulkan, and windowing, input and game files are handled by Android-specific host code.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Supported architectures
- arm64-v8a
- armeabi-v7a

## Installation
1. Install the APK.
2. Open **Black Android** and allow **all files access** when it asks (on Android 10, the **storage** permission).
3. Copy the game into `/storage/emulated/0/Black`, or into a `Black` folder at the top of your SD card: the USA or Europe release, either as a disc image (`.iso`) or as `default.xbe` with its data files.

If you don't grant that access, `Android/data/com.racas.black/files/assets` works too.

## System Requirements
- **Android** 10 or newer
- **CPU**: arm64-v8a or armeabi-v7a
- **GPU** with a Vulkan 1.1 driver, for example Adreno 6xx or newer, Mali-G7x or newer, or Xclipse. Drivers without dynamic rendering or timeline semaphores are supported.
- **Memory**: 4 GB.
- **Storage**: 2 GB for the game files and saves.

## Building
Building requires a Windows PC.

### 1. Clone the repository
```bash
git clone https://github.com/RanggaCasper/Black-Android.git
```

### 2. Add the game files
Put `default.xbe` from the USA release in `assets/`, or put the USA disc image (`.iso`) in `assets/` or `iso/`. The build extracts `default.xbe` from the image.

### 3. Install the dependencies
- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) with *Desktop development with C++*. The recompiler runs on the PC.
- [Android Studio](https://developer.android.com/studio). In its SDK Manager, install Android SDK Platform 34, NDK `28.2.13676358` and CMake `3.31.6`.
- [Python](https://www.python.org/), needed only to extract `default.xbe` from a disc image.

### 4. Build
Run `build.bat`. It builds the recompiler, recompiles the xbe into `generated/game`, then builds `app/build/outputs/apk/release/app-release.apk` with Gradle. The first build compiles the whole game and the runtime, so it takes 30 to 90 minutes.

Install the APK with:
```bash
adb install -r app/build/outputs/apk/release/app-release.apk
```
