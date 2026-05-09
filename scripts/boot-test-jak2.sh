#!/usr/bin/env bash
# boot-test-jak2.sh — automated boot smoke test for the Jak 2 mario port.
#
# What it does:
#   1. Make sure GAME=jak2 in scripts/tasks/.env (auto-switches if not).
#   2. Optionally rebuild gk/goalc (with --rebuild-cpp).
#   3. Run (mi) via the REPL to compile GOAL changes.
#   4. Launch gk in the background, wait BOOT_WAIT_SECS for it to settle.
#   5. Sample stdout for the "[mario] …" indicator strings that prove
#      every mario file loaded and ran its top-level init.
#   6. Kill gk regardless of result.
#   7. Print a green "PASS" / red "FAIL" line and exit 0 / 1.
#
# Usage:
#   scripts/boot-test-jak2.sh                  # GOAL-only test (default)
#   scripts/boot-test-jak2.sh --rebuild-cpp    # also rebuild gk/goalc first
#   scripts/boot-test-jak2.sh --boot-wait 45   # longer settle for slow boots
#   scripts/boot-test-jak2.sh --skip-mi        # skip (mi), just boot
#
# Exit codes:
#   0 = boot reached the mario load chain end without crashes
#   1 = a required indicator was missing or gk crashed
#   2 = build step failed before we could even try booting

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# ---------- defaults ----------
REBUILD_CPP=0
SKIP_MI=0
BOOT_WAIT_SECS=30
LOG_FILE=""
KEEP_LOG=0

# ---------- arg parsing ----------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --rebuild-cpp) REBUILD_CPP=1; shift;;
    --skip-mi)     SKIP_MI=1;     shift;;
    --boot-wait)   BOOT_WAIT_SECS="$2"; shift 2;;
    --log)         LOG_FILE="$2"; KEEP_LOG=1; shift 2;;
    -h|--help)
      sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
      exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

# Pick a log path if the user didn't.
if [[ -z "$LOG_FILE" ]]; then
  LOG_FILE="$(mktemp -t jak2-boot.XXXXXX.log)"
fi

# ---------- helpers ----------
RED=$'\033[31m'; GREEN=$'\033[32m'; YEL=$'\033[33m'; BLU=$'\033[34m'; RESET=$'\033[0m'
say()   { printf "%s[boot-test]%s %s\n"  "$BLU" "$RESET" "$*"; }
ok()    { printf "%s[boot-test]%s %s\n"  "$GREEN" "$RESET" "$*"; }
warn()  { printf "%s[boot-test]%s %s\n"  "$YEL" "$RESET" "$*"; }
fail()  { printf "%s[boot-test]%s %s\n"  "$RED"  "$RESET" "$*" >&2; }

cleanup() {
  # Kill gk if it's still around. -F to ensure a kill, not just a request.
  taskkill //F //IM gk.exe   >/dev/null 2>&1 || true
  taskkill //F //IM goalc.exe >/dev/null 2>&1 || true
  if [[ "$KEEP_LOG" -eq 0 ]]; then
    rm -f "$LOG_FILE"
  else
    say "log preserved at $LOG_FILE"
  fi
}
trap cleanup EXIT INT TERM

# ---------- step 1: ensure GAME=jak2 ----------
ENV_FILE="scripts/tasks/.env"
if ! grep -q '^GAME=jak2' "$ENV_FILE"; then
  warn "GAME != jak2 in $ENV_FILE — switching"
  task set-game-jak2 >/dev/null 2>&1 || { fail "task set-game-jak2 failed"; exit 2; }
fi
say "GAME=$(grep '^GAME=' "$ENV_FILE" | cut -d= -f2)"

# ---------- step 2 (optional): rebuild gk/goalc ----------
if [[ "$REBUILD_CPP" -eq 1 ]]; then
  say "rebuilding gk + goalc (Release-windows-clang-static)"
  taskkill //F //IM gk.exe   >/dev/null 2>&1 || true
  taskkill //F //IM goalc.exe >/dev/null 2>&1 || true
  if ! cmake --build out/build/Release --target gk goalc --parallel 8 >/dev/null 2>&1; then
    fail "gk/goalc rebuild failed"
    cmake --build out/build/Release --target gk goalc --parallel 8 2>&1 | tail -30
    exit 2
  fi
  ok "gk/goalc rebuilt"
fi

# ---------- step 3: run (mi) ----------
if [[ "$SKIP_MI" -eq 0 ]]; then
  say "running (mi) — compiling GOAL + packing iso"
  MI_OUT="$(printf '(mi)\n(e)\n' | task repl 2>&1 | tail -3)"
  if ! grep -q "Successfully built all" <<<"$MI_OUT"; then
    fail "(mi) failed:"
    printf '%s\n' "$MI_OUT" >&2
    exit 2
  fi
  TARGETS="$(grep -oE 'all [0-9]+ targets' <<<"$MI_OUT" | head -1)"
  ok "(mi) ${TARGETS:-built}"
fi

# ---------- step 4: launch gk ----------
say "launching gk_jak2 (boot wait: ${BOOT_WAIT_SECS}s)"
# Use task boot-game so the boot flags match what users see.
( task boot-game >"$LOG_FILE" 2>&1 ) &
GK_PID=$!
sleep "$BOOT_WAIT_SECS"

# ---------- step 5: check indicators ----------
declare -a REQUIRED=(
  "\[libsm64\] Initialized successfully"
  "link finish: mario-settings"
  "link finish: mario-menu-h"
  "link finish: mario"
  "link finish: mario-music"
  "\[mario\] applied settings:"
  "\[mario\] Jak 2 mario.gc loaded"
  "\[mario\] mario-music.gc loaded"
)

declare -a FORBIDDEN=(
  "ERROR: Compiler"
  "DECOMPILATION FAILED"
  "FATAL ERROR"
  "segmentation fault"
  "Microsoft C\+\+ exception"
  "object_already_loaded"
  "panic:"
)

# Pretty-print which mario lines we did / didn't see.
all_ok=1
for needle in "${REQUIRED[@]}"; do
  if grep -aqE "$needle" "$LOG_FILE"; then
    ok "found: $needle"
  else
    fail "MISSING: $needle"
    all_ok=0
  fi
done

for needle in "${FORBIDDEN[@]}"; do
  if grep -aqE "$needle" "$LOG_FILE"; then
    fail "FORBIDDEN substring present: $needle"
    grep -aE "$needle" "$LOG_FILE" | head -3 >&2
    all_ok=0
  fi
done

# ---------- step 6: report ----------
echo
if [[ "$all_ok" -eq 1 ]]; then
  ok "BOOT TEST PASSED — gk_jak2 reached title screen with mario chain loaded"
  exit 0
else
  fail "BOOT TEST FAILED"
  fail "tail of boot log:"
  tail -40 "$LOG_FILE" >&2
  if [[ "$KEEP_LOG" -eq 0 ]]; then
    KEEP_FAIL_LOG="$(mktemp -t jak2-boot-FAIL.XXXXXX.log)"
    cp "$LOG_FILE" "$KEEP_FAIL_LOG"
    fail "full log preserved at: $KEEP_FAIL_LOG"
  fi
  exit 1
fi
