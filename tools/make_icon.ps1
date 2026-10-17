# Generates res\lumen.ico (multi-size, PNG-compressed) from vector drawing code.
# Usage: powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
Add-Type -AssemblyName System.Drawing
$out = Join-Path $PSScriptRoot '..\res\lumen.ico'
$sizes = 256, 64, 48, 32, 24, 16
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.Clear([System.Drawing.Color]::Transparent)
    $pad = [Math]::Max(1, $s * 0.04)
    $r = $s - 2 * $pad
    $rad = $r * 0.23
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($pad, $pad, 2 * $rad, 2 * $rad, 180, 90)
    $path.AddArc($pad + $r - 2 * $rad, $pad, 2 * $rad, 2 * $rad, 270, 90)
    $path.AddArc($pad + $r - 2 * $rad, $pad + $r - 2 * $rad, 2 * $rad, 2 * $rad, 0, 90)
    $path.AddArc($pad, $pad + $r - 2 * $rad, 2 * $rad, 2 * $rad, 90, 90)
    $path.CloseFigure()
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, 0)), ([System.Drawing.PointF]::new($s, $s)), ([System.Drawing.Color]::FromArgb(255, 176, 108, 255)), ([System.Drawing.Color]::FromArgb(255, 84, 34, 178))
    $g.FillPath($grad, $path)
    # Soft top highlight
    $hl = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, 0)), ([System.Drawing.PointF]::new(0, $s + 2)), ([System.Drawing.Color]::FromArgb(70, 255, 255, 255)), ([System.Drawing.Color]::FromArgb(0, 255, 255, 255))
    $blend = New-Object System.Drawing.Drawing2D.Blend 3
    $blend.Factors = [float[]]@(0, 1, 1); $blend.Positions = [float[]]@(0, 0.6, 1)
    $hl.Blend = $blend
    $g.FillPath($hl, $path)
    # Ginger root: knobby body + two fingers (round-capped strokes), drawn twice for a soft shadow
    $knobs = @(
        @(0.50, 0.61, 0.54, 0.26), @(0.25, 0.57, 0.25, 0.25), @(0.75, 0.63, 0.24, 0.22), @(0.44, 0.73, 0.18, 0.12), @(0.52, 0.48, 0.22, 0.18))
    $fingers = @(
        @(0.27, 0.53, 0.21, 0.37, 0.17), @(0.56, 0.48, 0.64, 0.35, 0.15), @(0.79, 0.61, 0.88, 0.52, 0.12))
    function Draw-Ginger($brush, $dx, $dy) {
        foreach ($k in $knobs) {
            $g.FillEllipse($brush, [float](($k[0] - $k[2] / 2 + $dx) * $s), [float](($k[1] - $k[3] / 2 + $dy) * $s), [float]($k[2] * $s), [float]($k[3] * $s))
        }
        foreach ($f in $fingers) {
            $p = New-Object System.Drawing.Pen $brush, ([float]($f[4] * $s))
            $p.StartCap = 'Round'; $p.EndCap = 'Round'
            $g.DrawLine($p, [float](($f[0] + $dx) * $s), [float](($f[1] + $dy) * $s), [float](($f[2] + $dx) * $s), [float](($f[3] + $dy) * $s))
            $p.Dispose()
        }
    }
    $shadow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(90, 40, 10, 90))
    Draw-Ginger $shadow 0.0 0.03
    $skin = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, $s * 0.25)), ([System.Drawing.PointF]::new(0, $s * 0.85)), ([System.Drawing.Color]::FromArgb(255, 255, 233, 190)), ([System.Drawing.Color]::FromArgb(255, 222, 168, 98))
    Draw-Ginger $skin 0.0 0.0
    if ($s -ge 32) {
        # Growth rings across the knobs and fingers
        $ring = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(150, 160, 104, 46)), ([float][Math]::Max(1, $s * 0.018))
        $ring.StartCap = 'Round'; $ring.EndCap = 'Round'
        $arcs = @(@(0.16, 0.42, 0.10, 0.05), @(0.57, 0.40, 0.10, 0.05), @(0.34, 0.60, 0.13, 0.06), @(0.60, 0.60, 0.13, 0.06), @(0.80, 0.56, 0.09, 0.05), @(0.18, 0.60, 0.10, 0.05))
        foreach ($a in $arcs) {
            $g.DrawArc($ring, [float]($a[0] * $s), [float]($a[1] * $s), [float]($a[2] * $s), [float]($a[3] * $s), 20, 140)
        }
    }
    # Green sprout on the tall finger
    $leaf = New-Object System.Drawing.Drawing2D.GraphicsPath
    $lx = 0.21 * $s; $ly = 0.30 * $s
    $leaf.AddBezier([float]$lx, [float]$ly, [float]($lx - 0.02 * $s), [float]($ly - 0.12 * $s), [float]($lx + 0.10 * $s), [float]($ly - 0.16 * $s), [float]($lx + 0.15 * $s), [float]($ly - 0.13 * $s))
    $leaf.AddBezier([float]($lx + 0.15 * $s), [float]($ly - 0.13 * $s), [float]($lx + 0.12 * $s), [float]($ly - 0.04 * $s), [float]($lx + 0.06 * $s), [float]($ly + 0.01 * $s), [float]$lx, [float]$ly)
    $leaf.CloseFigure()
    $green = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, $ly - 0.16 * $s)), ([System.Drawing.PointF]::new(0, $ly)), ([System.Drawing.Color]::FromArgb(255, 160, 236, 120)), ([System.Drawing.Color]::FromArgb(255, 60, 170, 80))
    $g.FillPath($green, $leaf)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , $ms.ToArray()
    $bmp.Dispose()
}
$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$pngs[$i].Length); $bw.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Close()
Write-Output "ok: $out"
