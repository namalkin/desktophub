#!/usr/bin/env sh
# Сборка WidgetHub.exe через Docker (Linux / macOS / Git Bash).
# Контекст — корень репозитория: центру нужны README и скриншоты всех виджетов.
set -eu

cd "$(dirname "$0")/../.."

export DOCKER_BUILDKIT=1
docker build -f hub/Dockerfile --target export --output "type=local,dest=hub/dist" .

test -f hub/dist/WidgetHub.exe || { echo "hub/dist/WidgetHub.exe не появился" >&2; exit 1; }
ls -la hub/dist/WidgetHub.exe
echo "Готово: hub/dist/WidgetHub.exe"
