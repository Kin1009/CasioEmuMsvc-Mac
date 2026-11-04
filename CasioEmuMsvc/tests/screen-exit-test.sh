#!/bin/sh
# Regression test for the shutdown lifetime bug behind the SIGABRT on exit.
#
# It opens a real model plus a second session (CASIOEMU_OPEN_SECOND, the shell
# hook that creates an owned Emulator), waits long enough for the LCD/screen
# update workers to be running, then asks the process to leave through the
# normal main-loop teardown (`casioctl shutdown`, the same path as closing a
# window). A leaked worker thread used to lock the process-wide settings mutex
# after its static destructor had run, aborting with std::system_error.
#
# Usage: screen-exit-test.sh <CasioEmuMsvc executable> <casioctl> <model dir> [runs]
#
# Exit codes: 0 all runs exited cleanly, 1 failure, 77 no model available.

set -u

APP=${1:-}
CASIOCTL=${2:-}
MODEL=${3:-}
RUNS=${4:-3}

if [ -z "$APP" ] || [ ! -x "$APP" ]; then
    echo "screen-exit: missing CasioEmuMsvc executable" >&2
    exit 1
fi
if [ -z "$CASIOCTL" ] || [ ! -x "$CASIOCTL" ]; then
    echo "screen-exit: missing casioctl executable" >&2
    exit 1
fi
if [ -z "$MODEL" ] || [ ! -d "$MODEL" ]; then
    echo "screen-exit: SKIP: no model directory (set CASIOCTL_E2E_MODEL or pass one)"
    exit 77
fi

SCRATCH_ROOT=${SCREEN_EXIT_TEMP_ROOT:-"/Users/kin/Desktop/Wiz/temp/ui3/screen-exit"}
mkdir -p "$SCRATCH_ROOT" || exit 1
WORK="$SCRATCH_ROOT/run.$$"
mkdir -p "$WORK" || exit 1

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

# The executable resolves locales/roms.db and the second-session shell hook
# relative to its own directory, so run from there.
APP_DIR=$(cd "$(dirname "$APP")" && pwd)
cd "$APP_DIR" || exit 1

# Work on a copy so the run never mutates the source model (ram.dmp is written
# back on teardown).
MODEL_COPY="$WORK/model"
cp -R "$MODEL" "$MODEL_COPY" || exit 1

fail() {
    echo "screen-exit: FAIL: $*" >&2
    if [ -n "${LOG:-}" ] && [ -f "$LOG" ]; then
        echo "----- emulator log -----" >&2
        tail -60 "$LOG" >&2
        echo "------------------------" >&2
    fi
    exit 1
}

RUN=1
while [ "$RUN" -le "$RUNS" ]; do
    export CASIOEMU_CTL_DIR="$WORK/ctl-$RUN"
    # Selects SDL's dummy driver from a static initializer before main().
    export CASIOEMU_HEADLESS=1
    # Shell hook: open a second, owned Emulator session (leaked by ShutdownAll
    # in the old code) so a screen worker survives into process exit.
    export CASIOEMU_OPEN_SECOND=1

    LOG="$WORK/app-$RUN.log"
    "$APP" "model=$MODEL_COPY" "headless=" "paused=" >"$LOG" 2>&1 &
    APP_PID=$!

    READY=0
    for _ in $(seq 1 300); do
        if ! kill -0 "$APP_PID" 2>/dev/null; then
            APP_PID=
            fail "run $RUN: emulator exited during startup"
        fi
        if "$CASIOCTL" sessions 2>/dev/null | grep -q "^$APP_PID[[:space:]]"; then
            READY=1
            break
        fi
        sleep 0.1
    done
    [ "$READY" = 1 ] || fail "run $RUN: control session was not published within 30s"

    # Give the shell hook time to open the second session and both LCD workers
    # to enter their tick loops (TemporalTick + settings lease).
    sleep 2

    "$CASIOCTL" -s "$APP_PID" shutdown >"$WORK/shutdown-$RUN.log" 2>&1 || true

    EXITED=0
    for _ in $(seq 1 300); do
        if ! kill -0 "$APP_PID" 2>/dev/null; then
            EXITED=1
            break
        fi
        sleep 0.1
    done
    [ "$EXITED" = 1 ] || fail "run $RUN: process did not exit within 30s after shutdown"

    STATUS=0
    wait "$APP_PID" || STATUS=$?
    APP_PID=

    if [ "$STATUS" -ne 0 ]; then
        if [ "$STATUS" -eq 134 ] || [ "$STATUS" -eq 132 ] || [ "$STATUS" -eq 139 ]; then
            SIGNAL=$(kill -l "$((STATUS - 128))" 2>/dev/null || echo signal)
            fail "run $RUN: exit status $STATUS (SIG$SIGNAL); expected 0"
        fi
        fail "run $RUN: exit status $STATUS; expected 0"
    fi
    if grep -q "terminate called\|std::system_error\|libc++abi" "$LOG"; then
        fail "run $RUN: log reports an uncaught thread abort"
    fi
    echo "screen-exit: run $RUN/$RUNS exited 0"
    RUN=$((RUN + 1))
done

echo "screen-exit: all $RUNS runs exited cleanly"
exit 0
