#!/usr/bin/env bash
set -euo pipefail

APP_DIR="/home/elf/rk3568_capture"
QT_DIR="${APP_DIR}/qt_panel"
OUT_DIR="${QT_DIR}/_codex_generated_temp"
mkdir -p "${OUT_DIR}"

export DISPLAY=:0
export XAUTHORITY=/run/user/1000/.mutter-Xwaylandauth.HRBFQ3
export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus

start_panel() {
  local args=("$@")
  pgrep -x oct_qt_panel | xargs -r kill || true
  sleep 1
  cd "${APP_DIR}"
  nohup ./run_qt_panel.sh "${args[@]}" > /tmp/oct_qt_panel_run.log 2>&1 &
  sleep 4
  pgrep -af oct_qt_panel || {
    echo "oct_qt_panel failed to start" >&2
    tail -n 120 /tmp/oct_qt_panel_run.log >&2 || true
    exit 1
  }
}

find_window_id() {
  local pattern="$1"
  local tree_file="$2"
  xwininfo -root -tree > "${tree_file}"
  awk -v pat="${pattern}" 'index($0, pat) {print $1; exit}' "${tree_file}"
}

capture_window() {
  local pattern="$1"
  local stem="$2"
  local tree_file="${OUT_DIR}/${stem}_xwininfo.txt"
  local win_id
  win_id="$(find_window_id "${pattern}" "${tree_file}")"
  if [ -z "${win_id}" ]; then
    echo "window not found: ${pattern}" >&2
    sed -n '1,180p' "${tree_file}" >&2
    tail -n 120 /tmp/oct_qt_panel_run.log >&2 || true
    exit 1
  fi
  xwd -silent -id "${win_id}" -out "${OUT_DIR}/${stem}.xwd"
  python3 "${QT_DIR}/xwd_to_png.py" "${OUT_DIR}/${stem}.xwd" "${OUT_DIR}/${stem}.png"
  ls -l "${OUT_DIR}/${stem}.png"
}

start_panel --capture-tab
capture_window "OCT" "capture_panel"

start_panel --capture-tab --motor-dialog
capture_window "电机控制" "motor_control"

pgrep -x oct_qt_panel | xargs -r kill || true
echo "screenshots=${OUT_DIR}/capture_panel.png ${OUT_DIR}/motor_control.png"
