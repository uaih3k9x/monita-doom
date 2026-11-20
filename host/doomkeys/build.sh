#!/bin/sh
# 编 Mac 键盘转发器: build/doomkeys (见 host/doomkeys/main.swift 开头)
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build
swiftc -O -o build/doomkeys host/doomkeys/*.swift
echo "built build/doomkeys"
