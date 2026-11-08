[CmdletBinding()]
param([string[]]$Tasks=@(':app:assembleDebug',':app:testDebugUnitTest',':app:lintDebug'))
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$env:JAVA_HOME=Join-Path $projectRoot '.tools\jdk\jdk-21.0.13+9'
$env:ANDROID_HOME=Join-Path $projectRoot '.tools\android-sdk'
$env:ANDROID_USER_HOME=Join-Path $projectRoot '.tools\android-user'
$env:GRADLE_USER_HOME=Join-Path $projectRoot '.tools\gradle-user'
$env:PATH="$env:JAVA_HOME\bin;$env:ANDROID_HOME\platform-tools;$env:PATH"
$gradle=Join-Path $projectRoot '.tools\gradle-9.4.1\bin\gradle.bat'
if (!(Test-Path -LiteralPath $gradle)) { throw 'Run tools/setup_android.ps1 first' }
& $gradle -p (Join-Path $projectRoot 'app') --console=plain @Tasks
if ($LASTEXITCODE -ne 0) { throw 'Android build failed' }
