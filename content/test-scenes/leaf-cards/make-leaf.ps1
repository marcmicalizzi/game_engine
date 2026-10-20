# Writes leaf.tga beside this script: a procedural broadleaf leaf card for the leaf-cards scene
# (README.md). 512x512 RGBA, straight alpha: a pointed leaf along the image's u (the card's twig
# axis, flora.md "Materials") with a stem at u = 0, alpha 255 on the leaf and 0 off it with a
# one-texel soft edge, a green that darkens along a midrib, and black under the cut — what an
# editor exporting straight alpha leaves there, which the content build dilates (texture.md,
# "Alpha-tested masks"). TGA because it is twenty lines with no library; the engine decodes it
# like a PNG (foundation/image).
param([string]$Out = (Join-Path $PSScriptRoot 'leaf.tga'), [int]$Size = 512)
$n = $Size
$bytes = [byte[]]::new(18 + $n * $n * 4)
$bytes[2] = 2                                   # uncompressed true colour
$bytes[12] = $n -band 0xff; $bytes[13] = $n -shr 8
$bytes[14] = $n -band 0xff; $bytes[15] = $n -shr 8
$bytes[16] = 32                                 # BGRA
$bytes[17] = 0x28                               # 8 alpha bits, rows from the top
for ($y = 0; $y -lt $n; $y++) {
  $v = ($y + 0.5) / $n
  for ($x = 0; $x -lt $n; $x++) {
    $u = ($x + 0.5) / $n
    # The blade: half-width 0.24 * sin(pi * t) over t = (u - 0.12) / 0.86, so it is pointed at both ends.
    $t = ($u - 0.12) / 0.86
    $half = if ($t -gt 0 -and $t -lt 1) { 0.24 * [math]::Sin([math]::PI * $t) } else { 0.0 }
    $inside = ($half - [math]::Abs($v - 0.5)) * $n          # texels inside the edge
    if ($u -lt 0.14 -and [math]::Abs($v - 0.5) * $n -lt 3) { $inside = [math]::Max($inside, 1.0) }  # the stem
    $a = [math]::Min(1.0, [math]::Max(0.0, $inside + 0.5))
    $o = 18 + ($y * $n + $x) * 4
    if ($a -gt 0) {
      $rib = [math]::Exp(-[math]::Pow(($v - 0.5) * $n / 4.0, 2))
      $g = 0.55 + 0.25 * $u - 0.2 * $rib
      $bytes[$o + 0] = [byte](40 * $g)               # B
      $bytes[$o + 1] = [byte](200 * $g)              # G
      $bytes[$o + 2] = [byte](90 * $g)               # R
    }
    $bytes[$o + 3] = [byte]([math]::Round(255 * $a))
  }
}
[System.IO.File]::WriteAllBytes($Out, $bytes)
"wrote $Out"
