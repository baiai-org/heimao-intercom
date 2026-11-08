# Project-local Android toolchain. Does not replace the system Java installation.
[CmdletBinding()]
param([switch]$SkipSdkPackages)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$projectRoot=Split-Path -Parent $PSScriptRoot
$toolRoot=Join-Path $projectRoot '.tools'
$downloadRoot=Join-Path $toolRoot 'downloads'
New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
function Get-VerifiedArchive([string]$url,[string]$path,[string]$checksum,[string]$algorithm) {
    if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm $algorithm).Hash -ne $checksum) {
        Write-Host "Downloading $(Split-Path -Leaf $path)"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $path
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm $algorithm).Hash -ne $checksum) { throw "Checksum mismatch: $path" }
}
$jdkUrl='https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.13%2B9/OpenJDK21U-jdk_x64_windows_hotspot_21.0.13_9.zip'
$jdkHash='a4dbaa05fca4bd89d84584e58c459bd732efebb1722af80ab13c6516832004f9'
$jdkZip=Join-Path $downloadRoot 'temurin-21.0.13.zip'
$jdkRoot=Join-Path $toolRoot 'jdk'
if (!(Test-Path -LiteralPath "$jdkRoot\jdk-21.0.13+9\bin\java.exe")) {
    Get-VerifiedArchive $jdkUrl $jdkZip $jdkHash 'SHA256'
    Expand-Archive -LiteralPath $jdkZip -DestinationPath $jdkRoot -Force
}
$env:JAVA_HOME=Join-Path $jdkRoot 'jdk-21.0.13+9'
$env:PATH="$env:JAVA_HOME\bin;$env:PATH"
$env:ANDROID_HOME=Join-Path $toolRoot 'android-sdk'
$env:ANDROID_USER_HOME=Join-Path $toolRoot 'android-user'
$env:GRADLE_USER_HOME=Join-Path $toolRoot 'gradle-user'
New-Item -ItemType Directory -Force -Path $env:ANDROID_HOME,$env:ANDROID_USER_HOME,$env:GRADLE_USER_HOME | Out-Null
$sdkManager=Join-Path $env:ANDROID_HOME 'cmdline-tools\latest\bin\sdkmanager.bat'
if (!(Test-Path -LiteralPath $sdkManager)) {
    $repoPath=Join-Path $downloadRoot 'repository.xml'
    if (!(Test-Path -LiteralPath $repoPath)) { Invoke-WebRequest -UseBasicParsing -Uri 'https://dl.google.com/android/repository/repository2-3.xml' -OutFile $repoPath }
    [xml]$repo=Get-Content -LiteralPath $repoPath
    $package=$repo.SelectNodes("//*[local-name()='remotePackage']") | Where-Object { $_.GetAttribute('path') -eq 'cmdline-tools;23.0' } | Select-Object -First 1
    $archive=$package.archives.archive | Where-Object { $_.'host-os' -eq 'windows' } | Select-Object -First 1
    if (!$archive) { throw 'Official Android command-line tools 23.0 not found' }
    $sdkZip=Join-Path $downloadRoot 'android-commandline-23.0.zip'
    Get-VerifiedArchive ('https://dl.google.com/android/repository/'+$archive.complete.url) $sdkZip $archive.complete.checksum.InnerText 'SHA1'
    $extract=Join-Path $toolRoot 'sdk-extract'
    Expand-Archive -LiteralPath $sdkZip -DestinationPath $extract -Force
    $latest=Join-Path $env:ANDROID_HOME 'cmdline-tools\latest'
    New-Item -ItemType Directory -Force -Path $latest | Out-Null
    Get-ChildItem -LiteralPath "$extract\cmdline-tools" | Copy-Item -Destination $latest -Recurse -Force
}
$gradleRoot=Join-Path $toolRoot 'gradle-9.4.1'
if (!(Test-Path -LiteralPath "$gradleRoot\bin\gradle.bat")) {
    $hash=(Invoke-RestMethod -Uri 'https://downloads.gradle.org/distributions/gradle-9.4.1-bin.zip.sha256').Trim()
    $zip=Join-Path $downloadRoot 'gradle-9.4.1-bin.zip'
    Get-VerifiedArchive 'https://downloads.gradle.org/distributions/gradle-9.4.1-bin.zip' $zip $hash 'SHA256'
    Expand-Archive -LiteralPath $zip -DestinationPath $toolRoot -Force
}
# Pre-fetch the pinned large compiler artifact through the Windows downloader;
# this also avoids very slow Java HTTPS transfers on some Windows networks.
$compilerJar=Join-Path $downloadRoot 'kotlin-compiler-embeddable-2.4.10.jar'
$compilerHash='9309638a2ee03e6bde9ef4b7444055a94b84ab906563675d71ff9aecb64da913'
Get-VerifiedArchive 'https://repo.maven.apache.org/maven2/org/jetbrains/kotlin/kotlin-compiler-embeddable/2.4.10/kotlin-compiler-embeddable-2.4.10.jar' $compilerJar $compilerHash 'SHA256'
$compilerCache=Join-Path $env:GRADLE_USER_HOME 'caches\modules-2\files-2.1\org.jetbrains.kotlin\kotlin-compiler-embeddable\2.4.10\cac44a5a360313427c693a9de295017bc76d6e4e'
if (!(Test-Path -LiteralPath "$compilerCache\kotlin-compiler-embeddable-2.4.10.jar")) {
    New-Item -ItemType Directory -Force -Path $compilerCache | Out-Null
    Copy-Item -LiteralPath $compilerJar -Destination $compilerCache
}
if (!$SkipSdkPackages) {
    1..20 | ForEach-Object {'y'} | & $sdkManager "--sdk_root=$env:ANDROID_HOME" --licenses | Out-File (Join-Path $toolRoot 'sdk-licenses.log')
    # Command-line tools 23 uses SDK-style slash paths in its Windows wrapper.
    & $sdkManager "--sdk_root=$env:ANDROID_HOME" 'platform-tools' 'platforms/android-37.0' 'build-tools/36.0.0'
    if ($LASTEXITCODE -ne 0) { throw 'Android SDK package installation failed' }
    if (!(Test-Path -LiteralPath "$env:ANDROID_HOME\platforms\android-37.0\android.jar") -or
        !(Test-Path -LiteralPath "$env:ANDROID_HOME\build-tools\36.0.0\aapt2.exe")) { throw 'Required Android SDK packages were not installed' }
}
Write-Host "Android toolchain ready at $toolRoot"
