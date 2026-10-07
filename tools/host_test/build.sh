#!/usr/bin/env bash
# Builds the host test harness: the real component code (../../components/sky_tracker/*.h) with LVGL 9.5 rendering into
# a memory framebuffer, checked against reference data, writing PPM renders to out/.
#   ./build.sh            build ./t4
#   ./build.sh run        build, then run ./t4
# Env flags for ./t4 (renders under out/): ROCKET_PERF ROCKET_GIF ROCKET_INV ROCKET_SPIKES
#   UNDOCK_GIF VENUS_NOW ALERT_ICONS
set -euo pipefail
cd "$(dirname "$0")"
DEPS=deps
mkdir -p "$DEPS" out/renders build/lvgl
[ -f fixtures/saturn_test.jpg ] || python3 gen_fixtures.py  # synthetic picture fixtures (needs Pillow)
[ -d "$DEPS/lvgl" ] || git clone -q --depth 1 -b v9.5.0 https://github.com/lvgl/lvgl "$DEPS/lvgl"
[ -d "$DEPS/ArduinoJson" ] || git clone -q --depth 1 -b v7.4.3 https://github.com/bblanchon/ArduinoJson "$DEPS/ArduinoJson"
CC=${CC:-cc}
CXX=${CXX:-c++}
JOBS=${JOBS:-8}
CFLAGS="-O2 -w -DLV_CONF_INCLUDE_SIMPLE -I. -I$DEPS -I$DEPS/lvgl"
export CC CFLAGS
if [ ! -f build/liblvgl.a ] || [ lv_conf.h -nt build/liblvgl.a ]; then
  echo "building LVGL (once)..."
  find "$DEPS/lvgl/src" -name '*.c' | xargs -P "$JOBS" -I{} sh -c \
    'o=build/lvgl/$(echo "{}" | tr / _).o; [ -f "$o" ] && [ "$o" -nt lv_conf.h ] || $CC $CFLAGS -c "{}" -o "$o"'
  rm -f build/liblvgl.a
  ar rcs build/liblvgl.a build/lvgl/*.o
fi
for f in mdi14 mdi20 mdi40 mono16 mono18 mono24 tjpg/tjpgd; do
  o=build/$(basename $f).o
  [ -f "$o" ] && [ "$o" -nt $f.c ] || $CC $CFLAGS -c $f.c -o "$o"
done
echo "building t4..."
$CXX -std=gnu++20 $CFLAGS -Istubs -Itjpg -I$DEPS/ArduinoJson/src -I../../components/sky_tracker test4.cpp \
  build/mdi14.o build/mdi20.o build/mdi40.o build/mono16.o build/mono18.o build/mono24.o build/tjpgd.o \
  build/liblvgl.a -lm -lpthread -lz -o t4
if [ "${1:-}" = run ]; then ./t4; fi
