#!/bin/sh
# Builds the Android builder app (android/) into dist/ without Gradle: its
# resources (aapt2), its Java (javac against android.jar, then d8), and the
# builder program as its native library (Android/arm64 for phones, Linux/x86-64
# for emulators and Chromebooks), aligned and signed. Needs the JDK and
# Android SDK in ../tools-host and `make builder-templates` done; makes a
# signing key in ../tools-host/android-keystore the first time (keep it: the
# next versions must be signed with it to install over this one).
#   tools/make_android.sh
set -e
cd "$(dirname "$0")/.."
T=$(cd ../tools-host && pwd)
JAVA_HOME=${JAVA_HOME:-$T/jdk/Contents/Home}
PATH=$JAVA_HOME/bin:$PATH
export JAVA_HOME PATH
GO=${GO:-$T/go/bin/go}
SDK=${ANDROID_HOME:-$T/android-sdk}
BT=$SDK/build-tools/36.1.0
JAR=$SDK/platforms/android-36/android.jar
KEYSTORE=${KEYSTORE:-$T/android-keystore/platinum3hds.jks}
VERSION=$(sed -n 's/^const version = "\(.*\)"/\1/p' builder/main.go)
W=build-android
test -f builder/templates/template.cia.gz || { echo "run make builder-templates first"; exit 1; }

rm -rf "$W"
mkdir -p "$W/classes" "$W/apk/lib/arm64-v8a" "$W/apk/lib/x86_64" dist
(cd builder && CGO_ENABLED=0 GOOS=android GOARCH=arm64 "$GO" build -trimpath -ldflags "-s -w" -o "../$W/apk/lib/arm64-v8a/libplatinum3hds.so" .)
(cd builder && CGO_ENABLED=0 GOOS=linux GOARCH=amd64 "$GO" build -trimpath -ldflags "-s -w" -o "../$W/apk/lib/x86_64/libplatinum3hds.so" .)

"$BT/aapt2" compile --dir android/res -o "$W/res.zip"
"$BT/aapt2" link -o "$W/unaligned.apk" -I "$JAR" --manifest android/AndroidManifest.xml "$W/res.zip" \
    --min-sdk-version 24 --target-sdk-version 36 --version-name "$VERSION"
javac --release 11 -classpath "$JAR" -d "$W/classes" $(find android/src -name '*.java')
"$BT/d8" --release --min-api 24 --lib "$JAR" --output "$W/apk" $(find "$W/classes" -name '*.class')
(cd "$W/apk" && zip -q -r ../unaligned.apk classes.dex lib)
"$BT/zipalign" -f -P 16 4 "$W/unaligned.apk" "$W/aligned.apk"

if [ ! -f "$KEYSTORE" ]; then
    mkdir -p "$(dirname "$KEYSTORE")"
    (umask 077 && head -c 24 /dev/urandom | base64 > "$KEYSTORE.pass")
    keytool -genkeypair -keystore "$KEYSTORE" -alias platinum3hds -keyalg RSA -keysize 4096 -validity 36500 \
        -dname "CN=Platinum 3HDS" -storepass:file "$KEYSTORE.pass" -keypass:file "$KEYSTORE.pass"
fi
APK=dist/platinum3hds-builder-$VERSION-android.apk
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass "file:$KEYSTORE.pass" --out "$APK" "$W/aligned.apk"
"$BT/apksigner" verify "$APK"
echo "$APK"
