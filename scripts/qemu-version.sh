#!/bin/sh

set -eu

dir="$1"
pkgversion="$2"
version="$3"

if [ -z "$pkgversion" ]; then
    cd "$dir"
    if [ -e .git ] && command -v git >/dev/null 2>&1; then
        pkgversion=$(git describe --match 'v*' --dirty) || :
    fi
fi

if [ -n "$pkgversion" ]; then
    fullversion="$version ($pkgversion)"
else
    fullversion="$version"
fi

printf '%s\n' \
    "#define QEMU_PKGVERSION \"$pkgversion\"" \
    "#define QEMU_FULL_VERSION \"$fullversion\""
