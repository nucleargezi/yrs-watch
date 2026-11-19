#!/bin/sh
# Stage Ubuntu development headers for machines without sudo. The app uses
# the desktop's installed runtime libraries, exactly like a system-dev build.
set -eu
multiarch=$("${CXX:-g++}" -print-multiarch)
root=$(pwd)/build/deps
mkdir -p "$root/debs"
(
    cd "$root/debs"
    apt-get download libglib2.0-dev libgio-2.0-dev libjson-glib-dev libpango1.0-dev libharfbuzz-dev libcairo2-dev
    for archive in ./*.deb; do dpkg-deb -x "$archive" "$root"; done
)
cat > build/local-deps.mk.tmp <<END
WATCH_CPPFLAGS ?= -Ibuild/deps/usr/include/glib-2.0 -Ibuild/deps/usr/lib/$multiarch/glib-2.0/include -Ibuild/deps/usr/include/json-glib-1.0
WATCH_LIBS ?= -l:libgio-2.0.so.0 -l:libgobject-2.0.so.0 -l:libglib-2.0.so.0 -l:libjson-glib-1.0.so.0
FONT_CPPFLAGS ?= -Ibuild/deps/usr/include/glib-2.0 -Ibuild/deps/usr/lib/$multiarch/glib-2.0/include -Ibuild/deps/usr/include/pango-1.0 -Ibuild/deps/usr/include/harfbuzz -Ibuild/deps/usr/include/cairo
FONT_LIBS ?= -l:libpangocairo-1.0.so.0 -l:libpango-1.0.so.0 -l:libgobject-2.0.so.0 -l:libglib-2.0.so.0
END
mv build/local-deps.mk.tmp build/local-deps.mk
