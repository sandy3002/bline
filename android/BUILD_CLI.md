# Build the Android APK without Android Studio (Mac - fish shell)

You need the **Android SDK command-line tools** and a JDK. You do **not** need Android Studio.

This project is set up for **JDK 26** (Gradle 9.4.1 + AGP 9.2). That matches Homebrew OpenJDK on newer macOS.

## 1. JDK

Homebrew OpenJDK 26 is fine:

```fish
brew install openjdk
set -gx JAVA_HOME /opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home
java -version   # should show 26.x
```

Optional: JDK 21 from Adoptium’s website also works if you prefer an LTS install without Homebrew casks.

## 2. Android SDK

```fish
set -gx ANDROID_HOME $HOME/Library/Android/sdk
set -gx PATH $ANDROID_HOME/cmdline-tools/latest/bin $ANDROID_HOME/platform-tools $PATH
# Or add permanently to fish path:
#   fish_add_path $ANDROID_HOME/cmdline-tools/latest/bin $ANDROID_HOME/platform-tools

# if cmdline-tools missing:
#   brew install --cask android-commandlinetools

# Use Android CLI ('android sdk' is the modern replacement for 'sdkmanager'):
android sdk install "platforms;android-35"
android sdk install "build-tools;35.0.0"
android sdk install "platform-tools"

# Note: If falling back to deprecated sdkmanager:
#   yes | sdkmanager --licenses
#   sdkmanager "platforms;android-35" "build-tools;35.0.0"
```

## 3. Project local config

```fish
cd /Users/garchomp/Documents/Arduino/bline/android
echo "sdk.dir=$HOME/Library/Android/sdk" > local.properties
```

Gradle Wrapper is already in the repo (`./gradlew`). No need to run `gradle wrapper` again.

## 4. Build

```fish
cd /Users/garchomp/Documents/Arduino/bline/android
set -gx JAVA_HOME /opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home
./gradlew --stop
./gradlew assembleDebug
```

APK:

```
app/build/outputs/apk/debug/app-debug.apk
```

## 5. Install

```fish
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## Troubleshooting

| Problem | Fix |
|--------|-----|
| Error that is only `26.0.2` | Old Gradle 8.x — use this updated wrapper (9.4.1) |
| `SDK location not found` | Create `local.properties` with `sdk.dir=...` |
| Daemon using wrong Java | `./gradlew --stop` then `set -gx JAVA_HOME ...` again |
| First build slow | Normal; Gradle downloads once |
