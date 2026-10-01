param(
    [Parameter(Mandatory=$true)][string]$Sdk,
    [Parameter(Mandatory=$true)][string]$Jdk,
    [string]$Output = "$PSScriptRoot/build"
)
$ErrorActionPreference = 'Stop'
$Sdk = (Resolve-Path -LiteralPath $Sdk).Path
$Jdk = (Resolve-Path -LiteralPath $Jdk).Path
$Output = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$stage = Join-Path $Output ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage,(Join-Path $stage 'classes'),(Join-Path $stage 'dex') | Out-Null
$tools = Join-Path $Sdk 'build-tools/36.0.0'
$platform = Join-Path $Sdk 'platforms/android-36/android.jar'
function Run([string]$Tool,[string[]]$Arguments) {
    & $Tool @Arguments
    if($LASTEXITCODE -ne 0) { throw "$Tool failed ($LASTEXITCODE)" }
}
Run "$tools/aapt2.exe" @('compile','--dir',"$PSScriptRoot/res",'-o',"$stage/resources.zip")
Run "$tools/aapt2.exe" @('link','-o',"$stage/resources.apk",'--manifest',"$PSScriptRoot/AndroidManifest.xml",'-I',$platform,'--version-code','8','--version-name','0.5.3','--min-sdk-version','26','--target-sdk-version','36',"$stage/resources.zip")
$sources = @(Get-ChildItem "$PSScriptRoot/src" -Recurse -Filter *.java | ForEach-Object {$_.FullName})
Run "$Jdk/bin/javac.exe" (@('-source','8','-target','8','-Xlint:-options','-classpath',$platform,'-d',"$stage/classes") + $sources)
Run "$Jdk/bin/jar.exe" @('cf',"$stage/classes.jar",'-C',"$stage/classes",'.')
Run "$Jdk/bin/java.exe" @('-cp',"$tools/lib/d8.jar",'com.android.tools.r8.D8','--min-api','26','--lib',$platform,'--output',"$stage/dex", "$stage/classes.jar")
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open("$stage/resources.apk",[IO.Compression.ZipArchiveMode]::Update)
try {
    foreach($dex in Get-ChildItem "$stage/dex" -Filter *.dex) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$dex.FullName,$dex.Name) | Out-Null
    }
} finally { $zip.Dispose() }
Run "$tools/zipalign.exe" @('-f','-p','4',"$stage/resources.apk","$Output/Hyperlink-unsigned.apk")
Write-Output "Unsigned APK: $Output/Hyperlink-unsigned.apk"
Write-Output 'Sign with your private personal APK key before installation. Never publish that key.'
