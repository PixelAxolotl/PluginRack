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

# The app may still be running when the fresh executable is ready. Windows can
# also keep the image handle for a beat after the process exits (OneDrive or an
# antivirus scanning the new file), so matching the running process by path and
# retrying the copy is the difference between a transient failure and a broken
# build. Retry for ~10 s, then fail loudly rather than leave a stale artifact.
function Test-SamePath {
    param([string]$Left, [string]$Right)
    if (-not $Left -or -not $Right) { return $false }
    $leftPath = $Left -replace '^\\\\\?\\', ''
    $rightPath = $Right -replace '^\\\\\?\\', ''
    return [String]::Equals($leftPath, $rightPath, [StringComparison]::OrdinalIgnoreCase)
}

function Stop-PluginRackForPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stopped = 0
    $ids = @()
    try {
        $ids = @(Get-CimInstance Win32_Process -Filter "Name = 'PluginRack.exe'" -ErrorAction Stop |
            Where-Object { Test-SamePath -Left $_.ExecutablePath -Right $Path } |
            Select-Object -ExpandProperty ProcessId)
    }
    catch {
        $ids = @()
    }
    if ($ids.Count -eq 0) {
        # Fall back to the process object when CIM cannot report a path.
        $ids = @(Get-Process -Name "PluginRack" -ErrorAction SilentlyContinue |
            Where-Object { Test-SamePath -Left $_.Path -Right $Path } |
            Select-Object -ExpandProperty Id)
    }
    foreach ($processId in $ids) {
        Write-Output "Stopping running PluginRack.exe (PID $processId) to replace the executable"
        try {
            Stop-Process -Id $processId -Force -ErrorAction Stop
            $stopped++
        }
        catch {
            Write-Output "Warning: could not stop PluginRack.exe (PID $processId): $($_.Exception.Message)"
        }
    }
    return $stopped
}

function Copy-WithRetry {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [string]$ProcessPath = "",
        [int]$Attempts = 40,
        [int]$DelayMs = 250
    )
    $lastError = $null
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        try {
            Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop
            return
        }
        catch {
            $lastError = $_
            if ($ProcessPath) {
                [void](Stop-PluginRackForPath -Path $ProcessPath)
            }
            if ($attempt -lt $Attempts) {
                Start-Sleep -Milliseconds $DelayMs
            }
        }
    }
    throw "Could not replace $Destination after $Attempts attempts: $($lastError.Exception.Message)"
}

# Keep the opt-in WASAPI probe cap applied: `cmake --build` does not reconfigure,
# so a JUCE checkout that was reset or updated would silently lose the patch.
$cacheFile = Join-Path $repoRoot "build\dev\CMakeCache.txt"
if (Test-Path $cacheFile) {
    $juceLine = Select-String -Path $cacheFile -Pattern '^JUCE_DIR:PATH=(.+)$'
    if ($juceLine) {
        $juceDir = $juceLine.Matches[0].Groups[1].Value
        & $cmake "-DJUCE_DIR=$juceDir" `
                 "-DPATCH_FILE=$repoRoot\tools\juce-patches\0001-wasapi-probe-cap.patch" `
                 -P "$repoRoot\tools\juce-patches\apply-wasapi-probe-cap.cmake"
    }
}

$command = "call `"$vcVars`" >nul && cd /d `"$repoRoot`" && `"$cmake`" --build build\dev --parallel 4"
& $env:ComSpec /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path $builtExe)) {
    throw "Build completed but the executable was not found at $builtExe"
}

# Close an already-running copy first so the common case does not spend a retry
# on a predictable failure, then copy with the retry loop.
[void](Stop-PluginRackForPath -Path $rootExe)
Copy-WithRetry -Source $builtExe -Destination $rootExe -ProcessPath $rootExe

Write-Output "Copied $builtExe to $rootExe"

# Crash reports give a module offset; the matching PDB is what turns it into a
# function and line. Keep it beside the exe.
$builtPdb = [System.IO.Path]::ChangeExtension($builtExe, ".pdb")
$rootPdb = [System.IO.Path]::ChangeExtension($rootExe, ".pdb")
if (Test-Path $builtPdb) {
    try {
        Copy-WithRetry -Source $builtPdb -Destination $rootPdb -ProcessPath $rootExe -Attempts 8
        Write-Output "Copied $builtPdb to $rootPdb"
    }
    catch {
        Write-Output "Warning: could not copy $builtPdb ($($_.Exception.Message))"
    }
}
