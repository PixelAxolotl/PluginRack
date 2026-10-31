$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$vcVars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$builtExe = Join-Path $repoRoot "build\dev\app\pluginrack_app_artefacts\Release\PluginRack.exe"
$rootExe = Join-Path $repoRoot "PluginRack.exe"

if (-not (Test-Path $cmake)) {
    throw "CMake was not found at $cmake"
}
if (-not (Test-Path $vcVars)) {
    throw "MSVC environment script was not found at $vcVars"
}

$command = "call `"$vcVars`" >nul && cd /d `"$repoRoot`" && `"$cmake`" --build build\dev --parallel 4"
& $env:ComSpec /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path $builtExe)) {
    throw "Build completed but the executable was not found at $builtExe"
}

try {
    Copy-Item -LiteralPath $builtExe -Destination $rootExe -Force
}
catch {
    $runningProcesses = Get-CimInstance Win32_Process -Filter "Name = 'PluginRack.exe'" |
        Where-Object { $_.ExecutablePath -and
            [String]::Equals($_.ExecutablePath, $rootExe, [StringComparison]::OrdinalIgnoreCase) }

    if (-not $runningProcesses) {
        throw
    }

    foreach ($process in $runningProcesses) {
        Write-Output "Stopping running PluginRack.exe (PID $($process.ProcessId)) to replace the executable"
        Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop
    }

    Start-Sleep -Milliseconds 250
    Copy-Item -LiteralPath $builtExe -Destination $rootExe -Force
}

Write-Output "Copied $builtExe to $rootExe"
