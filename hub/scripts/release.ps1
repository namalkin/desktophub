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
# Не через конвейер PowerShell: PowerShell 5 искажает ввод внешней программы, и git
# отвечает «credential missing protocol field». Запрос — файлом с LF, через cmd.
$queryFile = [IO.Path]::GetTempFileName()
[IO.File]::WriteAllBytes($queryFile, [Text.Encoding]::ASCII.GetBytes("protocol=https`nhost=github.com`n`n"))
$env:GIT_TERMINAL_PROMPT = '0'
$cred = cmd /c "git credential fill < `"$queryFile`""
Remove-Item $queryFile -ErrorAction SilentlyContinue
$line = $cred | Where-Object { $_ -like 'password=*' } | Select-Object -First 1
$token = if ($line) { ([string]$line).Substring(9).Trim() } else { '' }
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
# Релиз с этим тегом уже есть (прошлый запуск оборвался) — дозаливаем в него.
$release = $null
try {
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/tags/$Tag" -Headers $headers
    Write-Host "Релиз $Tag уже есть — обновляю файлы в нём." -ForegroundColor Cyan
} catch {
    $release = Invoke-RestMethod -Method Post -Uri "https://api.github.com/repos/$Repo/releases" -Headers $headers `
        -Body ([Text.Encoding]::UTF8.GetBytes($body)) -ContentType 'application/json; charset=utf-8'
}

foreach ($f in $files) {
    $name = Split-Path $f -Leaf
    # Одноимённый файл в релизе заменяем: GitHub не даёт залить второй с тем же именем.
    foreach ($a in @($release.assets)) {
        if ($a -and $a.name -eq $name) {
            Invoke-RestMethod -Method Delete -Uri "https://api.github.com/repos/$Repo/releases/assets/$($a.id)" -Headers $headers | Out-Null
        }
    }
    Write-Host "  загружаю $name..."
    # Читаем с общим доступом: .exe может быть запущен, а -InFile открывает файл монопольно.
    $stream = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite')
    try {
        $data = New-Object byte[] $stream.Length
        [void]$stream.Read($data, 0, $data.Length)
    } finally {
        $stream.Dispose()
    }
    $uri = "https://uploads.github.com/repos/$Repo/releases/$($release.id)/assets?name=$name"
    Invoke-RestMethod -Method Post -Uri $uri -Headers $headers -Body $data -ContentType 'application/octet-stream' | Out-Null
}
Write-Host "Готово: $($release.html_url)" -ForegroundColor Green
