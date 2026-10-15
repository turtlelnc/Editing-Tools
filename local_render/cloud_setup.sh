#!/usr/bin/env bash
# Cloud Linux only. Uses the bundled MLT source; does not install OS packages.
set -euo pipefail
SCRIPT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_ROOT"
TASK_PREFIX="${MLT_CLOUD_PREFIX:-/tmp/mlt-7.42.0}"
TASK_BUILD="${MLT_CLOUD_BUILD:-/tmp/mlt-cloud-build}"
TASK_JOBS="${MLT_BUILD_JOBS:-2}"
mkdir -p "$TASK_BUILD"
if ! test -f "$TASK_BUILD/mlt/CMakeLists.txt"; then
 mkdir -p "$TASK_BUILD/mlt"
 tar -xzf "$SCRIPT_ROOT/vendor/mlt-v7.42.0.tar.gz" --strip-components=1 -C "$TASK_BUILD/mlt"
fi
mapfile -t MODULE_FLAGS < <(python3 - "$TASK_BUILD/mlt/CMakeLists.txt" <<'PY'
import re,sys
s=open(sys.argv[1]).read()
for name in re.findall(r'option\((MOD_[A-Z0-9_]+)\s',s):
 print('-D'+name+'='+('ON' if name in ['MOD_AVFORMAT','MOD_SOX','MOD_XML'] else 'OFF'))
PY
)
cmake -S "$TASK_BUILD/mlt" -B "$TASK_BUILD/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$TASK_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTING=OFF "${MODULE_FLAGS[@]}"
cmake --build "$TASK_BUILD/build" --parallel "$TASK_JOBS"
cmake --install "$TASK_BUILD/build"
export MLT_SYSTEM=1
export PKG_CONFIG_PATH="$TASK_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$TASK_PREFIX/lib:${LD_LIBRARY_PATH:-}"
export PATH="$TASK_PREFIX/bin:$PATH"
pkg-config --modversion mlt-framework-7
ffmpeg -version | sed -n '1,3p'
python3 local_render/run_all.py
