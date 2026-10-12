#!/usr/bin/env sh
# Сборка NewYearWidget.exe через Docker (Linux / macOS / Git Bash).
set -eu

cd "$(dirname "$0")/.."

export DOCKER_BUILDKIT=1
docker build --target export --output "type=local,dest=dist" .

test -f dist/NewYearWidget.exe || { echo "dist/NewYearWidget.exe не появился" >&2; exit 1; }
ls -la dist/NewYearWidget.exe
echo "Готово: dist/NewYearWidget.exe"
