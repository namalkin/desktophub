#!/usr/bin/env sh
# Сборка DiskVac.exe через Docker (Linux / macOS / Git Bash).
set -eu

cd "$(dirname "$0")/.."

export DOCKER_BUILDKIT=1
docker build --target export --output "type=local,dest=dist" .

test -f dist/DiskVac.exe || { echo "dist/DiskVac.exe не появился" >&2; exit 1; }
ls -la dist/DiskVac.exe
echo "Готово: dist/DiskVac.exe"
