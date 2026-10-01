$HyperlinkVideoArchiveSha='c73ad424c5f9d94dd48815b4d3531e57130cb9f5c3dc8f21f4aa4e1fdb0658e3'
$HyperlinkVideoExeSha='ee76bd5b4525289768485bd2db25993046c24127b3a9a332b02d2aca72377f59'
function Copy-HyperlinkVideoRuntime([string]$Destination) {
    $cache=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../vendor/ffmpeg'))
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    $archive=Join-Path $cache 'ffmpeg-N-127032-g6ae491a26c-win64-lgpl.zip'
    if(-not (Test-Path -LiteralPath $archive)){
        Invoke-WebRequest -Uri 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/ffmpeg-N-127032-g6ae491a26c-win64-lgpl.zip' -OutFile $archive -TimeoutSec 180
    }
    if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $HyperlinkVideoArchiveSha){throw 'Pinned video archive checksum mismatch'}
    $runtime=Join-Path $cache 'runtime'
    if(-not (Test-Path -LiteralPath $runtime)){Expand-Archive -LiteralPath $archive -DestinationPath $runtime}
    $exe=Get-ChildItem -LiteralPath $runtime -Filter ffmpeg.exe -Recurse | Select-Object -First 1
    if(-not $exe -or (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $HyperlinkVideoExeSha){throw 'Pinned FFmpeg executable checksum mismatch'}
    $root=$exe.Directory.Parent.FullName
    Copy-Item -LiteralPath $exe.FullName -Destination (Join-Path $Destination 'ffmpeg.exe') -Force
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE.txt') -Destination (Join-Path $Destination 'ffmpeg-LGPL.txt') -Force
    @'
FFmpeg LGPL static runtime: N-127032-g6ae491a26c, build 2026-09-30.
Upstream FFmpeg source: https://github.com/FFmpeg/FFmpeg/tree/6ae491a26c
Source archive: https://github.com/FFmpeg/FFmpeg/archive/6ae491a26c.zip
Producer release: https://github.com/BtbN/FFmpeg-Builds/releases/tag/autobuild-2026-09-30-13-08
Producer build recipes and dependency source references: https://github.com/BtbN/FFmpeg-Builds
Executable SHA-256: ee76bd5b4525289768485bd2db25993046c24127b3a9a332b02d2aca72377f59
The pinned producer binary is used as a separate process. A complete vendor source/rebuild bundle has not yet been validated.
'@ | Set-Content (Join-Path $Destination 'ffmpeg-source.txt')
}
