#!/usr/bin/env bash
set -euo pipefail

# ── config ─────────────────────────────────────────────────────────
LIB="./mymalloc.so"
TEST_BINS=(
    "tests/test_basic"
    "tests/test_coalesce"
    "tests/test_realloc"
    "tests/test_stress"
)
PASS=0
FAIL=0
ERRORS=()

# ── colours ────────────────────────────────────────────────────────
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# ── helpers ────────────────────────────────────────────────────────
pass() {
    PASS=$((PASS + 1))
    echo -e "${GREEN}PASS${NC} $1"
}

fail() {
    FAIL=$((FAIL + 1))
    ERRORS+=("$1")
    echo -e "${RED}FAIL${NC} $1"
}

section() {
    echo ""
    echo -e "${YELLOW}── $1 ──${NC}"
}

# ── sanity checks ──────────────────────────────────────────────────
if ! command -v valgrind &> /dev/null; then
    echo -e "${RED}error:${NC} valgrind not found"
    echo "  install with: sudo apt install valgrind"
    exit 1
fi

if [[ ! -f "$LIB" ]]; then
    echo -e "${RED}error:${NC} $LIB not found — run 'make' first"
    exit 1
fi

for bin in "${TEST_BINS[@]}"; do
    if [[ ! -f "$bin" ]]; then
        echo -e "${RED}error:${NC} $bin not found — run 'make test' first"
        exit 1
    fi
done

echo "=============================="
echo "  valgrind check"
echo "  lib: $LIB"
echo "=============================="

# ── valgrind flags ─────────────────────────────────────────────────
VALGRIND_FLAGS=(
    "--leak-check=full"
    "--track-origins=yes"
    "--error-exitcode=1"
    "--show-leak-kinds=all"
    "--errors-for-leak-kinds=definite,indirect"
)

# Optional: suppression file for syscalls (create if needed)
if [[ -f "valgrind_suppressions.txt" ]]; then
    VALGRIND_FLAGS+=("--suppressions=valgrind_suppressions.txt")
fi

# ── run each test binary ONCE ──────────────────────────────────────
section "memory error and leak checks"

for bin in "${TEST_BINS[@]}"; do
    echo -e "\n${CYAN}checking: $bin${NC}"
    
    LOGFILE=$(mktemp)
    
    # Run valgrind once
    if LD_PRELOAD="$LIB" valgrind "${VALGRIND_FLAGS[@]}" \
        "$bin" > "$LOGFILE" 2>&1; then
        pass "$bin — no errors, no leaks"
    else
        fail "$bin — valgrind reported errors"
        
        # Parse specific issues from the single log
        echo ""
        
        # Check for leaks
        if grep -q "definitely lost\|indirectly lost" "$LOGFILE"; then
            echo -e "  ${YELLOW} leaks detected:${NC}"
            grep -E "definitely lost|indirectly lost" "$LOGFILE" | sed 's/^/    /'
        fi
        
        # Check for invalid reads/writes
        if grep -q "Invalid read\|Invalid write" "$LOGFILE"; then
            echo -e "  ${YELLOW} invalid access detected:${NC}"
            grep -E "Invalid (read|write)" "$LOGFILE" | head -3 | sed 's/^/    /'
        fi
        
        # Check for use-after-free
        if grep -q "free'd\|Use of freed" "$LOGFILE"; then
            echo -e "  ${YELLOW} use-after-free detected:${NC}"
            grep -E "free'd|Use of freed" "$LOGFILE" | head -3 | sed 's/^/    /'
        fi
        
        # Check for double free
        if grep -q "double free\|Invalid free" "$LOGFILE"; then
            echo -e "  ${YELLOW} double free detected:${NC}"
            grep -E "double free|Invalid free" "$LOGFILE" | head -3 | sed 's/^/    /'
        fi
        
        # Check for uninitialized values
        if grep -q "Uninitialised" "$LOGFILE"; then
            echo -e "  ${YELLOW} uninitialized value detected:${NC}"
            grep "Uninitialised" "$LOGFILE" | head -2 | sed 's/^/    /'
        fi
        
        echo ""
    fi
    
    rm -f "$LOGFILE"
done

# ── summary ────────────────────────────────────────────────────────
echo ""
echo "=============================="
echo -e "  passed: ${GREEN}$PASS${NC}"
echo -e "  failed: ${RED}$FAIL${NC}"
echo "=============================="

if [[ ${#ERRORS[@]} -gt 0 ]]; then
    echo ""
    echo -e "${RED}failures:${NC}"
    for err in "${ERRORS[@]}"; do
        echo "  - $err"
    done
fi

echo ""

if [[ $FAIL -eq 0 ]]; then
    echo -e "${GREEN}all valgrind checks passed${NC}"
    exit 0
else
    echo -e "${RED}$FAIL check(s) failed${NC}"
    exit 1
fi