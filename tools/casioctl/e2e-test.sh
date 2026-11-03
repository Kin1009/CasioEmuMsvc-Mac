#!/bin/sh
# End-to-end test for the casioctl control channel.
#
# Usage: e2e-test.sh <CasioEmuMsvc executable> <casioctl> <model directory>
#
# Starts the emulator in offscreen/headless mode (SDL dummy video driver, no
# window server, no focus), discovers it with "casioctl sessions", and drives a
# scripted debugger session. Exits 0 on success, 77 when no model is available
# (CTest SKIP_RETURN_CODE), 1 on failure. Never leaves the emulator running.

set -u

APP=${1:-}
CASIOCTL=${2:-}
MODEL=${3:-}

if [ -z "$APP" ] || [ ! -x "$APP" ]; then
    echo "e2e: missing CasioEmuMsvc executable" >&2
    exit 1
fi
if [ -z "$CASIOCTL" ] || [ ! -x "$CASIOCTL" ]; then
    echo "e2e: missing casioctl executable" >&2
    exit 1
fi
if [ -z "$MODEL" ] || [ ! -d "$MODEL" ]; then
    echo "e2e: SKIP: no model directory (set CASIOCTL_E2E_MODEL or build the bundle models)"
    exit 77
fi

SCRATCH_ROOT=${CASIOCTL_TEMP_ROOT:-"$HOME/Desktop/Wiz/temp/casioctl"}
mkdir -p "$SCRATCH_ROOT" || exit 1
WORK=$(mktemp -d "$SCRATCH_ROOT/e2e.XXXXXX") || exit 1
APP_PID=
cleanup() {
    if [ -n "$APP_PID" ] && kill -0 "$APP_PID" 2>/dev/null; then
        kill "$APP_PID" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$APP_PID" 2>/dev/null || break
            sleep 0.2
        done
        kill -9 "$APP_PID" 2>/dev/null || true
        wait "$APP_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

export CASIOEMU_CTL_DIR="$WORK/ctl"
# Selects the dummy video driver from a static initializer that runs before
# main() calls SDL_Init; no WindowServer or display is required.
export CASIOEMU_HEADLESS=1

# The app resolves locales/roms.db relative to its own directory.
APP_DIR=$(cd "$(dirname "$APP")" && pwd)
cd "$APP_DIR" || exit 1

fail() {
    echo "e2e: FAIL: $*" >&2
    if [ -f "$WORK/app.log" ]; then
        echo "----- emulator log -----" >&2
        tail -40 "$WORK/app.log" >&2
        echo "------------------------" >&2
    fi
    exit 1
}

"$APP" "model=$MODEL" "headless=" "paused=" > "$WORK/app.log" 2>&1 &
APP_PID=$!

# Wait for the in-app control server to publish its session.
READY=0
for _ in $(seq 1 600); do
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        fail "emulator exited during startup"
    fi
    if "$CASIOCTL" sessions 2>/dev/null | grep -q "^$APP_PID[[:space:]]"; then
        READY=1
        break
    fi
    sleep 0.1
done
[ "$READY" = 1 ] || fail "control session was not published within 30s"

run() {
    "$CASIOCTL" -s "$APP_PID" "$@" || fail "command failed: $*"
}

# --- protocol / discovery ---------------------------------------------------
run hello | grep -q "debugger   yes" || fail "debugger interface unavailable"
run status | grep -q "state      paused" || fail "expected a paused session"

# --- registers / memory -----------------------------------------------------
run regs | grep -q "pc" || fail "register list is empty"
PC=$(run reg get pc | awk '{print $3}')
[ -n "$PC" ] || fail "cannot read pc"

ORIG=$(run read 0xE9E0 16 | awk 'NR==1 { for (i=2;i<=17;i++) printf "%s", $i }')
[ "${#ORIG}" -eq 32 ] || fail "read did not return 16 bytes (got '$ORIG')"
run write 0xE9E0 "$ORIG" | grep -q "wrote 16 byte" || fail "write failed"
AGAIN=$(run read 0xE9E0 16 | awk 'NR==1 { for (i=2;i<=17;i++) printf "%s", $i }')
[ "$ORIG" = "$AGAIN" ] || fail "memory write did not round-trip"
run inject 0xE9E0 "$ORIG" | grep -q "injected 16 byte" || fail "inject failed"

# --- disassembly / breakpoints / stepping ----------------------------------
ADDR=$(run disasm | awk '/^0x/{print $1; exit}')
[ -n "$ADDR" ] || fail "disassembly is empty"
run bp set "$ADDR" | grep -q "exec breakpoints" || fail "breakpoint set failed"
run bp list | grep -qi "$(printf '%s' "$ADDR" | sed 's/^0x//')" || fail "breakpoint missing from list"
run bp clear "$ADDR" > /dev/null || fail "breakpoint clear failed"
run step 2 | grep -q "stepped 2 instruction" || fail "step failed"
run step-over > /dev/null || fail "step-over failed"

# --- keyboard ---------------------------------------------------------------
run key list | grep -q "code 0x" || fail "key list is empty"
run key matrix 2 0 press > /dev/null || fail "key matrix press failed"
run key matrix 2 0 release > /dev/null || fail "key matrix release failed"
run key releaseall > /dev/null || fail "key releaseall failed"
# The "7" key exists on every calculator model; an unknown model is not fatal.
"$CASIOCTL" -s "$APP_PID" key press 7 > /dev/null 2>&1 || true
"$CASIOCTL" -s "$APP_PID" key release 7 > /dev/null 2>&1 || true

# --- internal screenshot (offscreen composited target) ---------------------
run screenshot "$WORK/calc.png" | grep -q "wrote" || fail "screenshot failed"
[ -s "$WORK/calc.png" ] || fail "screenshot file is empty"
MAGIC=$(dd if="$WORK/calc.png" bs=1 count=8 2>/dev/null | od -An -tx1 | tr -d ' \n')
[ "$MAGIC" = "89504e470d0a1a0a" ] || fail "screenshot is not a PNG (magic '$MAGIC')"

# --- snapshots / watches / call analysis -----------------------------------
SNAP=$(run snapshot save "e2e" | awk '{print $3}')
[ -n "$SNAP" ] || fail "snapshot save failed"
run snapshot list | grep -q "e2e" || fail "snapshot list missing entry"
run snapshot load "$SNAP" > /dev/null || fail "snapshot load failed"
run snapshot delete "$SNAP" > /dev/null || fail "snapshot delete failed"

run watch set 0xE9E0 42 | grep -q "locked" || fail "watch set failed"
run watch list | grep -q "0xE9E0" || fail "watch list missing entry"
run watch delete 0xE9E0 > /dev/null || fail "watch delete failed"

run call start > /dev/null || fail "call start failed"
run resume | grep -q "running" || fail "resume failed"
sleep 0.4
run pause | grep -q "paused" || fail "pause failed"
run call list > /dev/null || fail "call list failed"
run call stop > /dev/null || fail "call stop failed"

# --- hardware / analysis surfaces ------------------------------------------
run read-rom 0 8 | grep -q "ROM" || fail "read-rom failed"
run read-code 0 2 | grep -q "0x" || fail "read-code failed"
run hw | grep -q "hardware" || fail "hw failed"
run hw display | grep -q "residual_enabled" || fail "hw display failed"
run labels > /dev/null || fail "labels failed"
run vars > /dev/null || fail "vars failed"
run bt > /dev/null || fail "bt failed"
run bp mem 0xE9E0 1 write > /dev/null || fail "bp mem failed"
run bp mem clearall > /dev/null || fail "bp mem clearall failed"

echo "e2e: all checks passed"
exit 0
