# Выкладывает релиз на GitHub: центр и все виджеты, по одному .exe на каждый.
# Центр качает виджеты по адресу .../releases/latest/download/<Имя>.exe, поэтому имена
# файлов в релизе должны совпадать с полем "exe" в hub\catalog.json.
#
#   .\hub\scripts\release.ps1 -Tag v1.0.0 -DryRun   # только показать, что уйдёт
#   .\hub\scripts\release.ps1 -Tag v1.0.0
#
# gh не нужен: релиз создаётся через REST API GitHub. Токен берётся у git — тот самый
# вход, который Git Credential Manager сохранил при первом `git push`. Перед выпуском
# скрипт спрашивает у GitHub, чей это токен, и сверяет с владельцем, который ожидается.
# Сборку скрипт не запускает — сначала соберите центр и виджеты их build.ps1, а коммит,
# на который ставится тег, должен быть уже запушен.

param(
    [string]$Repo = '',             # по умолчанию — поле "repo" из catalog.json
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Account = 'namalkin',  # от чьего имени разрешено выпускать релиз
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$hub = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $hub
$catalog = Get-Content (Join-Path $hub 'catalog.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $Repo) { $Repo = $catalog.repo }

$files = @()
$missing = @()
foreach ($w in $catalog.widgets) {
    $exe = Join-Path $root (Join-Path $w.folder (Join-Path 'dist' $w.exe))
    if (Test-Path $exe) { $files += $exe } else { $missing += $exe }
}
$hubExe = Join-Path $hub 'dist\WidgetHub.exe'
if (Test-Path $hubExe) { $files += $hubExe } else { $missing += $hubExe }

if ($missing.Count -gt 0) {
    Write-Host 'Не собраны:' -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    throw 'Соберите недостающие .exe и повторите.'
}

Write-Host "Релиз $Tag в $Repo, файлы:" -ForegroundColor Cyan
$files | ForEach-Object { Write-Host ("  {0}  ({1} KB)" -f $_, [math]::Round((Get-Item $_).Length / 1KB)) }
if ($DryRun) { return }

# --- Токен из git credential ---
$query = "protocol=https`nhost=github.com`n`n"
$cred = $query | git credential fill 2>$null
$token = ($cred | Where-Object { $_ -like 'password=*' } | Select-Object -First 1) -replace '^password=', ''
if (-not $token) { throw 'git не хранит вход в github.com. Сделайте git push — Credential Manager попросит войти.' }

$headers = @{
    Authorization          = "Bearer $token"
    Accept                 = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
    'User-Agent'           = 'WidgetHub-release'
}

$me = Invoke-RestMethod -Uri 'https://api.github.com/user' -Headers $headers
if ($me.login -ne $Account) {
    throw "Сохранённый вход принадлежит '$($me.login)', а релиз должен выпускать '$Account'. Перелогиньтесь в git под $Account."
}
Write-Host "Аккаунт: $($me.login)" -ForegroundColor Cyan

# --- Релиз: тег ставится на текущий коммит ветки по умолчанию ---
$body = @{
    tag_name = $Tag
    name     = $Tag
    body     = 'Центр виджетов (WidgetHub.exe) и все виджеты серии. Достаточно скачать WidgetHub.exe — остальное он скачает сам.'
} | ConvertTo-Json
$release = Invoke-RestMethod -Method Post -Uri "https://api.github.com/repos/$Repo/releases" -Headers $headers `
    -Body ([Text.Encoding]::UTF8.GetBytes($body)) -ContentType 'application/json; charset=utf-8'

foreach ($f in $files) {
    $name = Split-Path $f -Leaf
    Write-Host "  загружаю $name..."
    $uri = "https://uploads.github.com/repos/$Repo/releases/$($release.id)/assets?name=$name"
    Invoke-RestMethod -Method Post -Uri $uri -Headers $headers -InFile $f -ContentType 'application/octet-stream' | Out-Null
}
Write-Host "Готово: $($release.html_url)" -ForegroundColor Green
