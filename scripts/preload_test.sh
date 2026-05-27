#!/usr/bin/env bash
set -euo pipefail

# ── config ─────────────────────────────────────────────────────────
LIB="./mymalloc.so"
PASS=0
FAIL=0
ERRORS=()

# Detect GNU coreutils binaries (with fallbacks)
GNU_LS="${GNU_LS:-/usr/bin/gnuls}"
GNU_CAT="${GNU_CAT:-/usr/bin/gnucat}"
GNU_WC="${GNU_WC:-/usr/bin/gnuwc}"
GNU_FIND="${GNU_FIND:-/usr/bin/gnufind}"
GNU_SORT="${GNU_SORT:-/usr/bin/gnusort}"
GNU_GREP="${GNU_GREP:-/usr/bin/gnugrep}"
GNU_SED="${GNU_SED:-/usr/bin/gnused}"
GNU_CP="${GNU_CP:-/usr/bin/gnucp}"

# Fallback to regular binaries if GNU versions don't exist
for bin in LS CAT WC FIND SORT GREP SED CP; do
    var="GNU_$bin"
    if [[ ! -f "${!var}" ]]; then
        fallback=$(which "$(echo "$bin" | tr '[:upper:]' '[:lower:]')" 2>/dev/null || echo "")
        if [[ -n "$fallback" && -f "$fallback" ]]; then
            eval "$var=\"$fallback\""
        fi
    fi
done

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

# Helper to run a command with LD_PRELOAD and check for crashes
run_with_preload() {
    local cmd="$1"
    local description="$2"
    
    output=$(LD_PRELOAD="$LIB" eval "$cmd" 2>&1)
    exit_code=$?
    
    # Check for crashes or memory errors in output
    if [[ $exit_code -eq 0 ]] && ! echo "$output" | grep -qiE "segmentation fault|core dumped|malloc|free|corrupt|invalid pointer"; then
        pass "$description"
        return 0
    else
        fail "$description (exit code: $exit_code)"
        if [[ -n "$output" ]]; then
            echo "  Last few lines of output:"
            echo "$output" | tail -3 | sed 's/^/    /'
        fi
        return 1
    fi
}

# ── sanity check ───────────────────────────────────────────────────
if [[ ! -f "$LIB" ]]; then
    echo -e "${RED}error:${NC} $LIB not found — run 'make' first"
    exit 1
fi

echo "=============================="
echo "  preload test"
echo "  lib: $LIB"
echo "=============================="

# ── test: library loads cleanly ────────────────────────────────────
section "library loading"

if LD_PRELOAD="$LIB" true 2>/dev/null; then
    pass "library loads without errors"
else
    fail "library failed to load"
    exit 1
fi

# ── test: basic unix tools ─────────────────────────────────────────
section "basic unix tools"

run_with_preload "$GNU_LS /tmp" "gnuls /tmp"
run_with_preload "echo 'hello world'" "echo"
run_with_preload "$GNU_CAT /etc/hostname" "gnucat /etc/hostname"
run_with_preload "$GNU_WC -l /etc/hostname" "gnuwc -l /etc/hostname"
run_with_preload "$GNU_FIND /tmp -maxdepth 2" "gnufind /tmp"
run_with_preload "echo -e 'b\na\nc' | $GNU_SORT" "gnusort"

# ── test: string processing ────────────────────────────────────────
section "string processing"

run_with_preload "$GNU_GREP localhost /etc/hosts" "gnugrep /etc/hosts"
run_with_preload "echo 'hello world' | $GNU_SED 's/world/allocator/'" "gnused substitution"
run_with_preload "echo '1 2 3' | awk '{print \$2}'" "awk field processing"

# ── test: file operations ──────────────────────────────────────────
section "file operations"

TMPFILE=$(mktemp)
run_with_preload "echo 'allocator test' > $TMPFILE" "write to file"
run_with_preload "$GNU_CAT $TMPFILE" "read from file"
run_with_preload "$GNU_CP $TMPFILE ${TMPFILE}.copy" "gnucp file"
rm -f "$TMPFILE" "${TMPFILE}.copy"

# ── test: null pointer free (important for allocators) ─────────────
section "null pointer handling"

# Create a small test program
NULL_TEST=$(mktemp)
cat > "$NULL_TEST.c" << 'EOF'
#include <stdlib.h>
int main() {
    free(NULL);  // Should not crash
    return 0;
}
EOF
gcc -o "$NULL_TEST" "$NULL_TEST.c" 2>/dev/null

if [[ -f "$NULL_TEST" ]]; then
    run_with_preload "$NULL_TEST" "free(NULL) handled gracefully"
    rm -f "$NULL_TEST" "$NULL_TEST.c"
fi

# ── test: python3 (if available) ───────────────────────────────────
section "python3 (if available)"

if command -v python3 &> /dev/null; then
    run_with_preload "python3 -c 'print(1 + 1)'" "python3 basic arithmetic"
    run_with_preload "python3 -c 'x = [i for i in range(10000)]'" "python3 list comprehension (10k items)"
    run_with_preload "python3 -c 's = \"hello \" * 1000; print(len(s))'" "python3 string allocation"
else
    echo "  skipped — python3 not found"
fi

# ── test: node (if available) ──────────────────────────────────────
section "node (if available)"

if command -v node &> /dev/null; then
    # Check if dynamically linked
    if ldd $(which node) &>/dev/null 2>&1; then
        run_with_preload "node -e 'console.log(\"hello from node\")'" "node basic execution"
    else
        echo "  skipped — node is statically linked, not compatible with LD_PRELOAD"
    fi
else
    echo "  skipped — node not found"
fi

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
    echo -e "${GREEN}all preload tests passed${NC}"
    exit 0
else
    echo -e "${RED}$FAIL test(s) failed${NC}"
    exit 1
fi