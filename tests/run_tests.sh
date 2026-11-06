#!/bin/sh
# 在 PC 上运行纯逻辑回归测试（不需要 ESP32 硬件）
#   用法: ./run_tests.sh
set -e
cd "$(dirname "$0")"

SRC=..
echo "编译测试程序..."
gcc -O1 -Wall -Wextra -o test_mc_logic \
    test_mc_logic.c \
    -I"$SRC/include" \
    "$SRC/main/registries.c"

echo "运行..."
./test_mc_logic
