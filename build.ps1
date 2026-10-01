param([string]$Output = (Join-Path $PSScriptRoot 'build'), [switch]$Test)
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'Hyperlink needs Windows with .NET Framework 4.8.' }
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$exe = Join-Path ([IO.Path]::GetFullPath($Output)) 'Hyperlink.exe'
$sources = @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'src') -Filter '*.cs' | Select-Object -ExpandProperty FullName)
. "$PSScriptRoot/scripts/AudioDependencies.ps1"
$audioReferences = @(Get-HyperlinkAudioReferences)
& $compiler /nologo /target:winexe /platform:x64 /optimize+ /warnaserror+ /out:$exe /win32icon:"$PSScriptRoot/assets/hyperlink.ico" /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll /reference:System.Security.dll /reference:System.Web.Extensions.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll @audioReferences $sources
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
Copy-HyperlinkAudioRuntime ([IO.Path]::GetDirectoryName($exe))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination $Output
New-Item -ItemType Directory -Path (Join-Path $Output 'docs') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'docs/ROADMAP.md') -Destination (Join-Path $Output 'docs')
if ($Test) {
    $report = Join-Path ([IO.Path]::GetFullPath($Output)) 'self-test.txt'
    $process = Start-Process -FilePath $exe -ArgumentList @('--self-test', ('"'+$report+'"')) -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0) { Get-Content -LiteralPath $report; throw 'Self-test failed.' }
    Get-Content -LiteralPath $report
}
Write-Output "Built $exe"
