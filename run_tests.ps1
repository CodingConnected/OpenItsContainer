# PowerShell script to run fast unit tests by default
# Usage: .\run_tests.ps1 [-BuildDir <dir>] [-Config Debug|Release] [-Integration] [-All]
# Examples:
#   .\run_tests.ps1 -BuildDir build-vs\x64-Debug-2.0.1
#   .\run_tests.ps1 -BuildDir build-vs\x64-Release-2.0.1 -Config Release -All

param(
    [string]$BuildDir = "build",
    [string]$Config = "Debug",
    [switch]$Integration,
    [switch]$All
)

$ctestCmd = "ctest"
$buildDir = $BuildDir

if (-not (Test-Path $buildDir)) {
    Write-Error "Build directory '$buildDir' not found. Run cmake first."
    exit 1
}

Push-Location $buildDir

try {
    if ($Integration) {
        Write-Host "Running integration tests..." -ForegroundColor Cyan
        & $ctestCmd -C $Config -L integration
    }
    elseif ($All) {
        Write-Host "Running all tests..." -ForegroundColor Cyan
        & $ctestCmd -C $Config
    }
    else {
        Write-Host "Running fast unit tests (excluding integration tests)..." -ForegroundColor Green
        & $ctestCmd -C $Config -LE integration
    }
}
finally {
    Pop-Location
}

