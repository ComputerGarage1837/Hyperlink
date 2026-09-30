# Generate the repository's code-drawn app icon without additional tooling.
Add-Type -AssemblyName System.Drawing
$bitmap = [Drawing.Bitmap]::new(256,256)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.Clear([Drawing.Color]::FromArgb(17,23,33))
$pen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(107,231,196),15)
$pen.StartCap = [Drawing.Drawing2D.LineCap]::Round
$pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
$graphics.DrawArc($pen,48,75,92,112,55,250)
$graphics.DrawArc($pen,112,75,92,112,235,250)
$graphics.DrawLine($pen,111,131,139,131)
$stream = [IO.MemoryStream]::new()
$bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
$png = $stream.ToArray()
$file = [IO.File]::Create((Join-Path $PSScriptRoot 'hyperlink.ico'))
$writer = [IO.BinaryWriter]::new($file)
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]1)
$writer.Write([byte]0); $writer.Write([byte]0); $writer.Write([byte]0); $writer.Write([byte]0)
$writer.Write([uint16]1); $writer.Write([uint16]32)
$writer.Write([uint32]$png.Length); $writer.Write([uint32]22); $writer.Write($png)
$writer.Dispose(); $stream.Dispose(); $pen.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
