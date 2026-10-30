# Builds res\logo.png (cropped sticker for the window) and res\icon.ico from the source art.
Add-Type -AssemblyName System.Drawing
$res = $PSScriptRoot

function Get-AlphaBounds([Drawing.Bitmap]$bmp) {
    $rect = New-Object Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
    $data = $bmp.LockBits($rect, 'ReadOnly', 'Format32bppArgb')
    $bytes = New-Object byte[] ($data.Stride * $bmp.Height)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    $minX = $bmp.Width; $minY = $bmp.Height; $maxX = -1; $maxY = -1
    for ($y = 0; $y -lt $bmp.Height; $y++) {
        $row = $y * $data.Stride
        for ($x = 0; $x -lt $bmp.Width; $x++) {
            if ($bytes[$row + $x * 4 + 3] -gt 8) {
                if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }
    New-Object Drawing.Rectangle $minX, $minY, ($maxX - $minX + 1), ($maxY - $minY + 1)
}

function New-Canvas([int]$w, [int]$h) {
    $bmp = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'; $g.SmoothingMode = 'HighQuality'
    $g.Clear([Drawing.Color]::Transparent)
    return $bmp, $g
}

$sticker = [Drawing.Bitmap]::FromFile("$res\sticker-source.png")
$bounds = Get-AlphaBounds $sticker
"sticker bounds: $bounds"

# Window logo: crop with a small margin.
$pad = 6
$crop = New-Object Drawing.Rectangle ($bounds.X - $pad), ($bounds.Y - $pad), ($bounds.Width + 2 * $pad), ($bounds.Height + 2 * $pad)
$bmp, $g = New-Canvas $crop.Width $crop.Height
$g.DrawImage($sticker, (New-Object Drawing.Rectangle 0, 0, $crop.Width, $crop.Height), $crop, 'Pixel')
$g.Dispose(); $bmp.Save("$res\logo.png", [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()

# Icon: centre the cropped art on a square, PNG-compressed entries.
$side = [Math]::Max($bounds.Width, $bounds.Height)
$sizes = 256, 128, 64, 48, 32, 24, 16
$pngs = foreach ($s in $sizes) {
    $bmp, $g = New-Canvas $s $s
    $scale = $s / $side
    $w = [int][Math]::Round($bounds.Width * $scale); $h = [int][Math]::Round($bounds.Height * $scale)
    $g.DrawImage($sticker, (New-Object Drawing.Rectangle ([int](($s - $w) / 2)), ([int](($s - $h) / 2)), $w, $h), $bounds, 'Pixel')
    $g.Dispose()
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    , $ms.ToArray()
}
$out = New-Object IO.MemoryStream
$bw = New-Object IO.BinaryWriter $out
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($png in $pngs) { $bw.Write($png) }
$bw.Flush(); [IO.File]::WriteAllBytes("$res\icon.ico", $out.ToArray())
$sticker.Dispose()
"wrote logo.png and icon.ico"
