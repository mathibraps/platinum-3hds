#!/bin/sh
# Builds the builder for each platform into dist/, each zipped with
# builder/README.txt (run `make builder-templates` first; the Android app is
# tools/make_android.sh):
#   tools/make_builder.sh
set -e
cd "$(dirname "$0")/.."
GO=${GO:-$(cd ../tools-host/go/bin && pwd)/go}
VERSION=$(sed -n 's/^const version = "\(.*\)"/\1/p' builder/main.go)
test -f builder/templates/template.cia.gz || { echo "run make builder-templates first"; exit 1; }
rm -rf dist
mkdir -p dist
for target in macos-arm64:darwin:arm64 macos-x64:darwin:amd64 linux-x64:linux:amd64 linux-arm64:linux:arm64 \
    windows-x64:windows:amd64 windows-arm64:windows:arm64; do
    name=${target%%:*}
    rest=${target#*:}
    os=${rest%%:*}
    arch=${rest#*:}
    exe=platinum3hds-builder
    [ "$os" = windows ] && exe=$exe.exe
    dir=platinum3hds-builder-$VERSION-$name
    mkdir -p "dist/$dir"
    (cd builder && CGO_ENABLED=0 GOOS=$os GOARCH=$arch "$GO" build -trimpath -ldflags "-s -w" -o "../dist/$dir/$exe" .)
    cp builder/README.txt "dist/$dir/"
    (cd dist && zip -q -r -X "$dir.zip" "$dir" && rm -rf "$dir")
    echo "dist/$dir.zip"
done
