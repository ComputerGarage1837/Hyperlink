function Get-HyperlinkRelayDependencies {
    $cache=Join-Path $PSScriptRoot '../vendor/java-websocket'
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $packages=@(
        @('Java-WebSocket-1.6.0.jar','org/java-websocket/Java-WebSocket/1.6.0','eae29213e4f16515639c28957200f011b3967fffcada1962cf0255d24919c22f'),
        @('slf4j-api-2.0.17.jar','org/slf4j/slf4j-api/2.0.17','7b751d952061954d5abfed7181c1f645d336091b679891591d63329c622eb832'),
        @('slf4j-nop-2.0.17.jar','org/slf4j/slf4j-nop/2.0.17','3716f83649ec66161a2edefd4f49df34d1dd1c51cdcf941996c6987260f0a829')
    )
    foreach($package in $packages){
        $file=Join-Path $cache $package[0]
        if(-not(Test-Path -LiteralPath $file)){Invoke-WebRequest "https://repo.maven.apache.org/maven2/$($package[1])/$($package[0])" -OutFile $file}
        if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $package[2]){throw "Relay dependency hash mismatch: $($package[0])"}
        (Resolve-Path -LiteralPath $file).Path
    }
}
