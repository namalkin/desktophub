# Сборка WidgetHub.exe через Docker Desktop.
# Запуск из любой папки:  .\hub\scripts\build.ps1
#
# Контекст сборки — корень репозитория: центр вшивает в себя README, скриншоты и иконки
# всех виджетов. Что именно уходит в Docker, решает hub\Dockerfile.dockerignore.

$ErrorActionPreference = 'Stop'

$hub = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent $hub
Set-Location $repo

Write-Host 'Проверяю Docker...' -ForegroundColor Cyan
docker version --format '{{.Server.Version}}' | Out-Null
if (-not $?) { throw 'Docker Desktop не запущен.' }

$env:DOCKER_BUILDKIT = '1'

Write-Host 'Собираю (первый раз — несколько минут, дальше из кэша)...' -ForegroundColor Cyan
docker build -f hub/Dockerfile --target export --output "type=local,dest=hub/dist" .
if (-not $?) { throw 'Сборка не удалась.' }

$exe = Join-Path $hub 'dist\WidgetHub.exe'
if (-not (Test-Path $exe)) { throw "Файл не появился: $exe" }

$sizeKb = [math]::Round((Get-Item $exe).Length / 1KB, 1)
Write-Host ''
Write-Host "Готово: $exe ($sizeKb KB)" -ForegroundColor Green
