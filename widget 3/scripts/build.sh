#!/usr/bin/env sh
# Сборка PlannerGo.exe через Docker (Linux / macOS / Git Bash).
set -eu

cd "$(dirname "$0")/.."

export DOCKER_BUILDKIT=1
docker build --target export --output "type=local,dest=dist" .

test -f dist/PlannerGo.exe || { echo "dist/PlannerGo.exe не появился" >&2; exit 1; }
ls -la dist/PlannerGo.exe
echo "Готово: dist/PlannerGo.exe"
