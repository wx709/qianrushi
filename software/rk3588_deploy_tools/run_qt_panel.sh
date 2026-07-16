#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_PATH="${SCRIPT_DIR}/qt_panel/build/oct_qt_panel"
LOCK_DIR="/tmp/oct_qt_panel.lock"
LOG_FILE="/tmp/oct_qt_panel_launch.log"

if [[ ! -x "${APP_PATH}" ]]; then
  echo "Qt panel binary not found: ${APP_PATH}"
  echo "Please run: ./build_qt_panel.sh"
  exit 1
fi

if ! mkdir "${LOCK_DIR}" 2>/dev/null; then
  old_pid="$(cat "${LOCK_DIR}/pid" 2>/dev/null || true)"
  if [[ -n "${old_pid}" ]] && [[ -r "/proc/${old_pid}/comm" ]] &&
     [[ "$(cat "/proc/${old_pid}/comm" 2>/dev/null)" = "oct_qt_panel" ]] &&
     kill -0 "${old_pid}" 2>/dev/null; then
    if command -v wmctrl >/dev/null 2>&1; then
      DISPLAY="${DISPLAY:-:0}" XAUTHORITY="${XAUTHORITY:-/run/user/1000/gdm/Xauthority}" \
        wmctrl -xa oct_qt_panel.oct_qt_panel 2>/dev/null || true
    fi
    exit 0
  fi
  rm -rf "${LOCK_DIR}"
  mkdir "${LOCK_DIR}"
fi
echo "$$" > "${LOCK_DIR}/pid"
trap 'rm -rf "${LOCK_DIR}"' EXIT

export GENICAM_ROOT_V3_0=/usr/local/bin/camcmosoctusb3/genicam3_0_2
export GENICAM_ROOT="${GENICAM_ROOT_V3_0}"
export GENICAM_GENTL64_PATH=/usr/local/lib/camcmosoctusb3_1.2

if [[ "${HOME:-/}" = "/" ]]; then
  export GENICAM_CACHE_V3_0=/.config/teledyne_e2v/genicam_cache_v3_0
else
  export GENICAM_CACHE_V3_0="${HOME}/.config/teledyne_e2v/genicam_cache_v3_0"
fi
export GENICAM_CACHE="${GENICAM_CACHE_V3_0}"
mkdir -p "${GENICAM_CACHE}"

export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
export QT_XCB_GL_INTEGRATION="${QT_XCB_GL_INTEGRATION:-none}"
export QT_OPENGL="${QT_OPENGL:-software}"
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"

export LD_LIBRARY_PATH="${GENICAM_ROOT_V3_0}/bin/Linux64_ARM:${GENICAM_GENTL64_PATH}:${LD_LIBRARY_PATH:-}"

echo "[$(date '+%F %T')] launching ${APP_PATH}" >> "${LOG_FILE}"
exec "${APP_PATH}" "$@" >> "${LOG_FILE}" 2>&1
