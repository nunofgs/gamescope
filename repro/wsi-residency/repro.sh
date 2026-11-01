#!/bin/sh
set -eu
cd "$(dirname "$0")"
cc -std=c11 -O2 -Wall -Wextra -Werror image-probe.c -o image-probe $(pkg-config --cflags --libs vulkan)
cc -shared -fPIC -O2 -Wall -Wextra -Werror trace.c -o trace.so $(pkg-config --cflags libdrm) -ldl -lpthread
glslangValidator -V sample.comp -o sample.spv
python3 run.py
