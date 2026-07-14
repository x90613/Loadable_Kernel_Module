#!/bin/bash
# Full integration test for loadable_kernel_module.
# Must be run as root inside the Linux VM after: make && make generateTestFile
# Usage: sudo bash test_src/run_tests.sh

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)

DEV=/dev/loadable_kernel_module
MOD=loadable_kernel_module
PASS=0
FAIL=0
FAILED_CASES=()

pass() { echo "[PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL + 1)); FAILED_CASES+=("$1"); }

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "Error: must run as root"
        exit 1
    fi
}

cleanup() {
    echo ""
    echo "--- Cleanup ---"
    pkill -f "$REPO_ROOT/out/hsuckd" 2>/dev/null || true
    pkill -f "$REPO_ROOT/out/NTUST"  2>/dev/null || true
    pkill -f "$REPO_ROOT/out/MIT"    2>/dev/null || true

    # Restore syscall table before rmmod if hooks were installed
    # (rmmod triggers lkm_exit which restores originals)
    if lsmod | grep -q "^$MOD "; then
        rmmod $MOD 2>/dev/null || true
    fi

    rm -f $DEV
}
trap cleanup EXIT

# ---------------------------------------------------------------
require_root

echo "=== loadable_kernel_module integration tests ==="
echo ""

# --- Setup ---
echo "--- Setup ---"

if ! lsmod | grep -q "^$MOD "; then
    insmod $REPO_ROOT/${MOD}.ko
fi

MAJOR=$(dmesg | grep "major number for your device" | tail -1 | grep -o '[0-9]*$')
if [ -z "$MAJOR" ]; then
    echo "Error: could not read major number from dmesg"
    exit 1
fi
echo "major number: $MAJOR"

rm -f $DEV
mknod $DEV c "$MAJOR" 0
chmod 666 $DEV

echo ""

# --- Test 0: IOCTL_MOD_HIDE (toggle module visibility) ---
echo "--- Test 0: module hide / unhide ---"

lsmod | grep -q "^$MOD " && pass "module visible in lsmod before hide" \
                           || fail "module not visible in lsmod before hide"

$REPO_ROOT/out/userTest 0

if lsmod | grep -q "^$MOD "; then
    fail "module still visible after hide"
else
    pass "module hidden from lsmod"
fi

$REPO_ROOT/out/userTest 0

lsmod | grep -q "^$MOD " && pass "module visible again after unhide" \
                           || fail "module not visible after unhide"

echo ""

# --- Test 1: IOCTL_MOD_MASQ (process name masquerade) ---
echo "--- Test 1: process masquerade ---"

$REPO_ROOT/out/NTUST &
NTUST_PID=$!
$REPO_ROOT/out/MIT &
MIT_PID=$!

# Wait until both processes appear in the task list
for i in $(seq 1 10); do
    ps -p $NTUST_PID -o comm= 2>/dev/null | grep -q "NTUST" && break
    sleep 0.1
done

ps -p $NTUST_PID -o comm= 2>/dev/null | grep -q "NTUST" \
    && pass "NTUST process visible before masq" \
    || fail "NTUST process not found before masq"

ps -p $MIT_PID -o comm= 2>/dev/null | grep -q "MIT" \
    && pass "MIT process visible before masq" \
    || fail "MIT process not found before masq"

$REPO_ROOT/out/userTest 1

# NTUST → NTU (shorter: should succeed)
ps -p $NTUST_PID -o comm= 2>/dev/null | grep -q "NTU" \
    && pass "NTUST renamed to NTU" \
    || fail "NTUST not renamed to NTU"

# MIT → Standford (longer: should be skipped)
ps -p $MIT_PID -o comm= 2>/dev/null | grep -q "MIT" \
    && pass "MIT unchanged (longer new name correctly skipped)" \
    || fail "MIT was incorrectly renamed"

kill $NTUST_PID $MIT_PID 2>/dev/null || true
wait $NTUST_PID $MIT_PID 2>/dev/null || true

echo ""

# --- Test 2: IOCTL_MOD_HOOK (syscall hooks) ---
echo "--- Test 2: syscall hooks ---"

$REPO_ROOT/out/hsuckd &
HSUCKD_PID=$!
sleep 0.2

$REPO_ROOT/out/userTest 2

# kill -9 should be intercepted — process survives
kill -9 $HSUCKD_PID 2>/dev/null || true
sleep 0.2

if kill -0 $HSUCKD_PID 2>/dev/null; then
    pass "hsuckd survived SIGKILL (kill hook active)"
else
    fail "hsuckd was killed — kill hook did not intercept SIGKILL"
fi

# kill -10 (SIGUSR1) should pass through — process dies
kill -10 $HSUCKD_PID 2>/dev/null || true
sleep 0.2

if kill -0 $HSUCKD_PID 2>/dev/null; then
    fail "hsuckd still alive after SIGUSR1 (should have passed through)"
else
    pass "hsuckd terminated by SIGUSR1 (non-9 signal passes through)"
fi

echo ""

# --- Test 3: IOCTL_FILE_HIDE (hide file from ls) ---
echo "--- Test 3: file hide ---"

ls $REPO_ROOT | grep -q "HiddenFile" \
    && pass "HiddenFile visible before hide" \
    || fail "HiddenFile not found before hide (check working directory)"

$REPO_ROOT/out/userTest 3

if ls $REPO_ROOT | grep -q "HiddenFile"; then
    fail "HiddenFile still visible after hide"
else
    pass "HiddenFile hidden from ls"
fi

echo ""

# --- Summary ---
echo "=== Results: $PASS passed, $FAIL failed ==="
if [ $FAIL -ne 0 ]; then
    echo "Failed cases:"
    for case in "${FAILED_CASES[@]}"; do
        echo "  - $case"
    done
    exit 1
fi
