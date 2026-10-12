# Генерирует res/app.ico (снежинка на акцентном фоне) во всех нужных размерах.
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

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    # Скруглённый квадрат с вертикальным градиентом.
    $radius = [math]::Max(2, [int]($size * 0.22))
    $rect = New-Object System.Drawing.Rectangle(0, 0, ($size - 1), ($size - 1))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc(($rect.Right - $d), $rect.Y, $d, $d, 270, 90)
    $path.AddArc(($rect.Right - $d), ($rect.Bottom - $d), $d, $d, 0, 90)
    $path.AddArc($rect.X, ($rect.Bottom - $d), $d, $d, 90, 90)
    $path.CloseFigure()

    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 122, 150, 255),
        [System.Drawing.Color]::FromArgb(255, 74, 96, 224),
        90.0)
    $g.FillPath($brush, $path)

    # Снежинка: 6 лучей с боковыми ответвлениями.
    $cx = $size / 2.0
    $cy = $size / 2.0
    $r = $size * 0.32
    $penWidth = [math]::Max(1.0, $size * 0.075)
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, $penWidth)
    $pen.StartCap = 'Round'
    $pen.EndCap = 'Round'

    for ($i = 0; $i -lt 6; $i++) {
        $a = [math]::PI / 3.0 * $i
        $ex = $cx + [math]::Cos($a) * $r
        $ey = $cy + [math]::Sin($a) * $r
        $g.DrawLine($pen, [single]$cx, [single]$cy, [single]$ex, [single]$ey)

        if ($size -ge 24) {
            foreach ($at in @(0.55, 0.85)) {
                $bx = $cx + [math]::Cos($a) * $r * $at
                $by = $cy + [math]::Sin($a) * $r * $at
                $len = $r * 0.28
                foreach ($off in @(0.6, -0.6)) {
                    $tx = $bx + [math]::Cos($a + $off) * $len
                    $ty = $by + [math]::Sin($a + $off) * $len
                    $g.DrawLine($pen, [single]$bx, [single]$by, [single]$tx, [single]$ty)
                }
            }
        }
    }

    $pen.Dispose(); $brush.Dispose(); $path.Dispose(); $g.Dispose()
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
