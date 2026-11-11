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

function Stop-RunningApp {
    param([string[]] $ExePaths)

    $wanted = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $ExePaths) { [void]$wanted.Add($path) }

    $running = @(Get-CimInstance Win32_Process -Filter "Name = 'PluginRack.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -and $wanted.Contains($_.ExecutablePath) })

    # PR-SEC-014: never kill silently. Only the exact built/root paths match
    # (already scoped above), and the user confirms before anything stops.
    if ($running.Count -gt 0) {
        Write-Host "Running PluginRack.exe instance(s) hold the executable open:"
        foreach ($process in $running) {
            Write-Host "  PID $($process.ProcessId): $($process.ExecutablePath)"
        }
        $answer = Read-Host "Stop them so the build can replace the exe? [y/N]"
        if ($answer -ne "y" -and $answer -ne "Y") {
            throw "Build aborted: close the running PluginRack.exe instance(s) and run again."
        }
        foreach ($process in $running) {
            Write-Host "Stopping PID $($process.ProcessId)"
            Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
        }
    }

    return $running.Count
}

function Wait-ForFileRelease {
    param([string] $Path, [int] $TimeoutSeconds = 15)

    if (-not (Test-Path $Path)) {
        return $true
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $stream = [System.IO.File]::Open(
                $Path,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None)
            $stream.Dispose()
            return $true
        }
        catch [System.IO.IOException] {
            Start-Sleep -Milliseconds 250
        }
        catch [System.UnauthorizedAccessException] {
            Start-Sleep -Milliseconds 250
        }
    }

    return $false
}

# A running instance holds the exe open: one started from the build output breaks
# the link step, one started from the repo root breaks the copy below.
$stopped = [int](Stop-RunningApp -ExePaths @($rootExe, $builtExe))
if ($stopped -gt 0) {
    Write-Output "Closed $stopped running instance(s) of PluginRack.exe"
}
if (-not (Wait-ForFileRelease -Path $builtExe)) {
    throw "The build output $builtExe is still locked by another process"
}

$command = "call `"$vcVars`" >nul && cd /d `"$repoRoot`" && `"$cmake`" --build build\dev --parallel 4"
& $env:ComSpec /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path $builtExe)) {
    throw "Build completed but the executable was not found at $builtExe"
}

# The app may have been relaunched during the build, so close it again right
# before the copy instead of failing on a locked file.
[void](Stop-RunningApp -ExePaths @($rootExe))
if (-not (Wait-ForFileRelease -Path $rootExe)) {
    throw "PluginRack.exe is still locked by another process; close it and run the script again"
}

Copy-Item -LiteralPath $builtExe -Destination $rootExe -Force

Write-Output "Copied $builtExe to $rootExe"

# Crash reports give a module offset; the matching PDB is what turns it into a
# function and line. PR-SEC-015: keep PDBs off the release folder (they hand
# an attacker function names and line numbers) — stash them under
# build\symbols instead, which is where a symbol server would pick them up.
$builtPdb = [System.IO.Path]::ChangeExtension($builtExe, ".pdb")
$symbolsDir = Join-Path $repoRoot "build\symbols"
if (Test-Path $builtPdb) {
    try {
        New-Item -ItemType Directory -Path $symbolsDir -Force | Out-Null
        Copy-Item -LiteralPath $builtPdb -Destination (Join-Path $symbolsDir "PluginRack.pdb") -Force
        Write-Output "Copied $builtPdb to $symbolsDir\PluginRack.pdb"
    }
    catch {
        Write-Output "Warning: could not copy $builtPdb ($($_.Exception.Message))"
    }
}
