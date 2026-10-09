# Convert the original generated PNG candidates into Windows ICO containers.
# The art is unchanged; each ICO embeds standard PNG sizes with alpha intact.
param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$folder = Join-Path (Split-Path -Parent $PSScriptRoot) 'assets\ui\launcher\icons'
foreach ($file in Get-ChildItem -LiteralPath $folder -Filter '*.png' -File) {
    $source = [Drawing.Image]::FromFile($file.FullName)
    $frames = @()
    foreach ($size in @(16,24,32,48,64,128,256)) {
        $bitmap = [Drawing.Bitmap]::new($size,$size,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.DrawImage($source,[Drawing.Rectangle]::new(0,0,$size,$size))
        $stream = [IO.MemoryStream]::new()
        $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
        $frames += [pscustomobject]@{ Size=$size; Bytes=$stream.ToArray() }
        $stream.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
    }
    $source.Dispose()
    $output = [IO.File]::Create([IO.Path]::ChangeExtension($file.FullName,'.ico'))
    $writer = [IO.BinaryWriter]::new($output)
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        $sizeByte = if ($frame.Size -eq 256) { 0 } else { $frame.Size }
        $writer.Write([byte]$sizeByte); $writer.Write([byte]$sizeByte)
        $writer.Write([byte]0); $writer.Write([byte]0); $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frame.Bytes.Length); $writer.Write([uint32]$offset)
        $offset += $frame.Bytes.Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame.Bytes) }
    $writer.Dispose()
    Write-Output ($file.BaseName + ': seven alpha-preserving icon sizes')
}
