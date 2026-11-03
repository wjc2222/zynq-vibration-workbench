#!/usr/bin/env bash
set -euo pipefail
src="$(cd "$(dirname "$0")" && pwd)"
out="${1:?usage: build_candidate.sh NEW_ASCII_OUTPUT KERNEL_BUILD CROSS_PREFIX}"
kernel="${2:?kernel build required}"
cross="${3:?ARM cross prefix required}"
test ! -e "$out"
mkdir -p "$out/module/include" "$out/bin"
out="$(cd "$out" && pwd)"
test "$(cat "$kernel/include/config/kernel.release")" = 6.6.70-ax7z020-offline
cp "$src/ax7z020_accel.c" "$out/module/"
cp "$src/include/"*.h "$out/module/include/"
printf '%s\n' 'obj-m += ax7z020_accel.o' 'ccflags-y += -I$(src)/include' > "$out/module/Makefile"
cc -std=c11 -O2 -Wall -Wextra -Werror -I"$src/include" "$src/tests/credit_policy_test.c" -o "$out/bin/credit-policy-test"
"$out/bin/credit-policy-test" | tee "$out/credit-policy-test.log"
make -C "$kernel" M="$out/module" ARCH=arm CROSS_COMPILE="$cross" modules -j4
c++ -std=c++17 -O2 -Wall -Wextra -Werror -I"$src/include" "$src/tests/complete_result_probe.cpp" -o "$out/bin/complete-result-probe-host"
"$out/bin/complete-result-probe-host" --self-test | tee "$out/complete-probe-self-test.log"
"${cross}g++" -std=c++17 -O2 -Wall -Wextra -Werror -I"$src/include" "$src/tests/complete_result_probe.cpp" -o "$out/bin/v7-complete-result-probe"
file "$out/module/ax7z020_accel.ko" "$out/bin/v7-complete-result-probe"
sha256sum "$out/module/ax7z020_accel.ko" "$out/bin/v7-complete-result-probe" > "$out/runtime.sha256"
