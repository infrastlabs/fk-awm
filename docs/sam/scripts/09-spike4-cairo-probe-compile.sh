#!/bin/sh
# 09-spike4-cairo-probe-compile.sh — probe-compile lgishim_cairo.c in isolation
#
# Why not `ninja`? The full build pulls in the `lgi-check` custom target, which
# fails in this container (no lgi runtime) and needs AWESOME_IGNORE_LGI=1. For a
# fast "did my edit compile?" loop it is much cheaper to compile the one
# translation unit directly with the same flags/include paths CMake would use.
#
# FLAGS / INCLUDES are copied verbatim from build.ninja's
# `build CMakeFiles/awesome.dir/lib/vendor/lgishim_cairo.c.o:` stanza. They drift
# when CMake config changes, so after re-running `cmake` diff them by hand.
#
# NOTE: do NOT try to extract these from build.ninja with awk/grep inside the
# container -- the single-quoted regexes get mangled by the shell and awk bails
# with "Unmatched ( or \(". Hardcoding is the reliable path.
#
# Usage: bash 09-spike4-cairo-probe-compile.sh [output.o]
#        (default output: /tmp/lgishim_cairo_probe.o)
set -u

REPO=${REPO:-/_ext/home/headless/xm-zs01/fk-awesomewm}
SRC="$REPO/lib/vendor/lgishim_cairo.c"
OBJ=${1:-/tmp/lgishim_cairo_probe.o}

FLAGS='-O3 -DNDEBUG -O1 -std=gnu99 -ggdb3 -fno-strict-aliasing -Wall -Wextra -Wchar-subscripts -Wundef -Wshadow -Wcast-align -Wwrite-strings -Wsign-compare -Wunused -Wno-unused-parameter -Wuninitialized -Winit-self -Wpointer-arith -Wformat-nonliteral -Wno-format-zero-length -Wmissing-format-attribute -Wmissing-prototypes -Wstrict-prototypes'
INC='-I/_ext/tmp/awe-build-static -I/usr/include/glib-2.0 -I/usr/lib/glib-2.0/include -I/usr/local/include/gdk-pixbuf-2.0 -I/usr/include/libpng16 -I/usr/include/libmount -I/usr/include/blkid -I/usr/include/cairo -I/usr/include/pixman-1 -I/usr/include/freetype2 -I/usr/include/uuid -I/usr/include/startup-notification-1.0 -I/usr/local/include/dbus-1.0 -I/usr/local/lib/dbus-1.0/include -I/usr/include/pango-1.0 -I/usr/include/harfbuzz -I/usr/include/fribidi'

# shellcheck disable=SC2086
gcc -Dawesome_EXPORTS $FLAGS $INC -c "$SRC" -o "$OBJ" -MD -MF "$OBJ.d"
rc=$?

echo "--- $SRC -> $OBJ (rc=$rc)"
exit "$rc"
