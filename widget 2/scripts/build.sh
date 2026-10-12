#!/usr/bin/env sh
# Сборка ShortcutDock.exe через Docker (Linux / macOS / Git Bash).
set -eu

cd "$(dirname "$0")/.."

export DOCKER_BUILDKIT=1
docker build --target export --output "type=local,dest=dist" .

test -f dist/ShortcutDock.exe || { echo "dist/ShortcutDock.exe не появился" >&2; exit 1; }
ls -la dist/ShortcutDock.exe
echo "Готово: dist/ShortcutDock.exe"
