# Сборка PlannerGo.exe через Docker Desktop.
# Запуск из корня проекта:  .\scripts\build.ps1

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

Write-Host 'Проверяю Docker...' -ForegroundColor Cyan
docker version --format '{{.Server.Version}}' | Out-Null
if (-not $?) { throw 'Docker Desktop не запущен.' }

$env:DOCKER_BUILDKIT = '1'

Write-Host 'Собираю (первый раз — несколько минут, дальше из кэша)...' -ForegroundColor Cyan
docker build --target export --output "type=local,dest=dist" .
if (-not $?) { throw 'Сборка не удалась.' }

$exe = Join-Path $root 'dist\PlannerGo.exe'
if (-not (Test-Path $exe)) { throw "Файл не появился: $exe" }

$sizeKb = [math]::Round((Get-Item $exe).Length / 1KB, 1)
Write-Host ''
Write-Host "Готово: $exe ($sizeKb KB)" -ForegroundColor Green
Write-Host 'Запуск: двойной клик по файлу.' -ForegroundColor Green
