$HyperlinkAudioHashes = @{
    'naudio.core'='b374dce05e9928b0a6ba6a73458b598395e3bd43553ae8c7e82a5572b275924fbaae38892ed93363316628895437dd1ed2c0ca59f819ba47678e16e3bbdc8256'
    'naudio.wasapi'='31b7418eb02523342b54dbe7cb4bcff8eb89dbb869ead0f7b52884ad2e1e1f85f13ffcd61233e4c8a6f7d19bcf1bd2c290753c1ac8d52e9c677078648c930109'
}
function Get-HyperlinkAudioReferences {
    $cache=Join-Path $PSScriptRoot '../vendor/naudio'
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    foreach($name in @('naudio.core','naudio.wasapi')){
        $package=Join-Path $cache "$name.2.3.0.nupkg"
        if(-not(Test-Path -LiteralPath $package)){
            Invoke-WebRequest -TimeoutSec 30 -Uri "https://api.nuget.org/v3-flatcontainer/$name/2.3.0/$name.2.3.0.nupkg" -OutFile $package
        }
        if((Get-FileHash -LiteralPath $package -Algorithm SHA512).Hash.ToLowerInvariant() -ne $HyperlinkAudioHashes[$name]){throw "Audio package checksum mismatch: $name"}
        Copy-Item -LiteralPath $package -Destination "$package.zip" -Force
        Expand-Archive -LiteralPath "$package.zip" -DestinationPath (Join-Path $cache $name) -Force
    }
    $standard=Get-ChildItem "$env:WINDIR/Microsoft.NET/assembly/GAC_MSIL/netstandard" -Recurse -Filter netstandard.dll | Select-Object -First 1 -ExpandProperty FullName
    if(-not $standard){throw '.NET Framework 4.8 netstandard facade is required'}
    @(( '/reference:'+ $standard),
      ( '/reference:'+ (Resolve-Path "$cache/naudio.core/lib/netstandard2.0/NAudio.Core.dll").Path),
      ( '/reference:'+ (Resolve-Path "$cache/naudio.wasapi/lib/netstandard2.0/NAudio.Wasapi.dll").Path))
}
function Copy-HyperlinkAudioRuntime([string]$Destination){
    $cache=Join-Path $PSScriptRoot '../vendor/naudio'
    Copy-Item -LiteralPath "$cache/naudio.core/lib/netstandard2.0/NAudio.Core.dll","$cache/naudio.wasapi/lib/netstandard2.0/NAudio.Wasapi.dll","$PSScriptRoot/../licenses/NAudio-MIT.txt" -Destination $Destination -Force
}
