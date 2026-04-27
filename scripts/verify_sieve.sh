#!/bin/bash
# Verify prime_sieve.hpp correctness with 2B+ integers.
# Runs after any modification to the shared sieve implementation.
# Expected: π(2×10⁹) = 98,222,287

set -euo pipefail
cd "$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)"

echo "[HOOK] Sieve verification: rebuilding..."
./build.sh -r glm5_seastar_prime 2>&1 | tail -1

echo "[HOOK] Running 2B integer verification..."
OUTPUT=$(./build/release/glm5_seastar_prime -t 10000 -n 200000 -c 4 --logger-ostream-type none 2>/dev/null)
PRIMES=$(echo "$OUTPUT" | grep "素数总数" | grep -o '[0-9]*' | tail -1)
MS=$(echo "$OUTPUT" | grep "计算耗时" | grep -o '[0-9]*' | tail -1)

echo "[HOOK] Range: 2-2,000,000,000 | Primes: $PRIMES | Time: ${MS}ms"

if [ "$PRIMES" != "98222287" ]; then
    echo "[HOOK] FAIL: expected 98222287, got $PRIMES"
    exit 1
fi

echo "[HOOK] PASS: prime count verified"
