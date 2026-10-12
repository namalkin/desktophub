# Генерирует res/app.ico (полка с ярлыками) во всех нужных размерах.
# Запускать только при изменении дизайна иконки — результат коммитится в репозиторий,
# потому что сборка идёт в Linux-контейнере, где System.Drawing недоступен.
#
#   .\scripts\make-icon.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$outPath = Join-Path $root 'res\app.ico'

# Мелкие размеры пишем классическим DIB (его понимают все загрузчики), а 256x256 —
# PNG: несжатый он весил бы 256 КБ и раздул бы .exe.
$dibSizes = @(16, 24, 32, 48)
$pngSizes = @(256)
$sizes = $dibSizes + $pngSizes

function Add-RoundedRect($path, [double]$x, [double]$y, [double]$w, [double]$h, [double]$r) {
    $d = $r * 2
    if ($d -gt $w) { $d = $w }
    if ($d -gt $h) { $d = $h }
    $path.AddArc([single]$x, [single]$y, [single]$d, [single]$d, 180, 90)
    $path.AddArc([single]($x + $w - $d), [single]$y, [single]$d, [single]$d, 270, 90)
    $path.AddArc([single]($x + $w - $d), [single]($y + $h - $d), [single]$d, [single]$d, 0, 90)
    $path.AddArc([single]$x, [single]($y + $h - $d), [single]$d, [single]$d, 90, 90)
    $path.CloseFigure()
}

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    # Скруглённый квадрат с вертикальным градиентом — та же форма, что у виджета 1,
    # но другой цвет: два виджета должны различаться в трее с первого взгляда.
    $rect = New-Object System.Drawing.Rectangle(0, 0, ($size - 1), ($size - 1))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    Add-RoundedRect $path 0 0 ($size - 1) ($size - 1) ([math]::Max(2, [int]($size * 0.22)))

    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 156, 122, 255),
        [System.Drawing.Color]::FromArgb(255, 106, 72, 214),
        90.0)
    $g.FillPath($brush, $path)

    # Полка: две строки плиток — так читается «группы ярлыков».
    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $dim = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(170, 255, 255, 255))

    $margin = $size * 0.22
    $inner = $size - $margin * 2
    $gap = [math]::Max(1.0, $size * 0.06)
    $tile = ($inner - $gap) / 2.0
    $radius = [math]::Max(1.0, $tile * 0.28)

    for ($row = 0; $row -lt 2; $row++) {
        for ($col = 0; $col -lt 2; $col++) {
            $x = $margin + $col * ($tile + $gap)
            $y = $margin + $row * ($tile + $gap)
            $tilePath = New-Object System.Drawing.Drawing2D.GraphicsPath
            Add-RoundedRect $tilePath $x $y $tile $tile $radius
            # Одна плитка приглушена — «свёрнутая группа».
            $fill = if ($row -eq 1 -and $col -eq 1) { $dim } else { $white }
            $g.FillPath($fill, $tilePath)
            $tilePath.Dispose()
        }
    }

    $white.Dispose(); $dim.Dispose(); $brush.Dispose(); $path.Dispose(); $g.Dispose()
    return $bmp
}

# Иконочный DIB: заголовок с удвоенной высотой, снизу вверх BGRA, следом маска AND.
function Get-DibBytes([System.Drawing.Bitmap]$bmp) {
    $w = $bmp.Width
    $h = $bmp.Height
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)

    $bw.Write([uint32]40)          # biSize
    $bw.Write([int32]$w)           # biWidth
    $bw.Write([int32]($h * 2))     # biHeight: XOR + AND
    $bw.Write([uint16]1)           # biPlanes
    $bw.Write([uint16]32)          # biBitCount
    $bw.Write([uint32]0)           # BI_RGB
    $bw.Write([uint32]($w * $h * 4))
    $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)

    for ($y = $h - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $w; $x++) {
            $c = $bmp.GetPixel($x, $y)
            $bw.Write([byte]$c.B); $bw.Write([byte]$c.G); $bw.Write([byte]$c.R); $bw.Write([byte]$c.A)
        }
    }

    # Маска AND: прозрачность задаёт альфа-канал, поэтому маска нулевая, но нужна по формату.
    $maskRow = [math]::Floor(($w + 31) / 32) * 4
    $zeros = New-Object byte[] ($maskRow * $h)
    $bw.Write($zeros)

    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()
    return , $bytes   # запятая обязательна: иначе PowerShell развернёт массив в поток байтов
}

$payloads = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    if ($pngSizes -contains $s) {
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $payloads += , $ms.ToArray()
        $ms.Dispose()
    } else {
        $payloads += , (Get-DibBytes $bmp)
    }
    $bmp.Dispose()
}

$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($out)
$w.Write([uint16]0)                 # reserved
$w.Write([uint16]1)                 # type: icon
$w.Write([uint16]$sizes.Count)

$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))   # 0 означает 256
    $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $w.Write([byte]0)               # палитра не используется
    $w.Write([byte]0)               # reserved
    $w.Write([uint16]1)             # planes
    $w.Write([uint16]32)            # bpp
    $w.Write([uint32]$payloads[$i].Length)
    $w.Write([uint32]$offset)
    $offset += $payloads[$i].Length
}
foreach ($p in $payloads) { $w.Write([byte[]]$p) }
$w.Flush()

[System.IO.File]::WriteAllBytes($outPath, $out.ToArray())
$w.Dispose(); $out.Dispose()

Write-Host "Готово: $outPath ($((Get-Item $outPath).Length) байт, размеры: $($sizes -join ', '))"
