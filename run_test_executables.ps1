# PowerShell script to run all test executables directly
# Usage: .\run_test_executables.ps1 [-BuildDir <dir>] [-Config Debug|Release]
# Examples:
#   .\run_test_executables.ps1 -BuildDir build-vs\x64-Debug-2.0.1
#   .\run_test_executables.ps1 -BuildDir build-vs\x64-Release-2.0.1 -Config Release

param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug"
)

$buildDir = "$BuildDir\tests\$Config"
$testPattern = "test_*.exe"

# Check if build directory exists
if (-not (Test-Path $buildDir)) {
    Write-Error "Test directory '$buildDir' not found. Build the tests first."
    exit 1
}

# Find all test executables
$testExecutables = Get-ChildItem -Path $buildDir -Filter $testPattern -File

if ($testExecutables.Count -eq 0) {
    Write-Warning "No test executables found matching pattern '$testPattern' in '$buildDir'"
    exit 0
}

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "Running $($testExecutables.Count) test executable(s) in $Config mode" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

$failedTests = @()
$totalTests = $testExecutables.Count
$passedTests = 0
$failedCount = 0

# Run each test executable
foreach ($testExe in $testExecutables) {
    $testName = $testExe.Name
    $testPath = $testExe.FullName
    
    Write-Host "`n[Running] $testName" -ForegroundColor Yellow
    Write-Host "----------------------------------------" -ForegroundColor DarkGray
    
    $output = @()
    $hasFailure = $false
    $processOutput = ""
    
    try {
        # Run the test and capture output
        # Use & operator to run directly and capture both stdout and stderr
        $allOutput = @()
        
        # Capture output line by line and display in real-time
        & $testPath 2>&1 | ForEach-Object {
            $line = $_
            $allOutput += $line
            Write-Host $line
        }
        
        # Get exit code (PowerShell 7+ uses $LASTEXITCODE, older versions may need different approach)
        $exitCode = 0
        if ($null -ne $LASTEXITCODE) {
            $exitCode = $LASTEXITCODE
        }
        
        # Convert output to string for analysis
        $processOutput = $allOutput | Out-String
        
        # Check for failures in output
        if ($processOutput -match "\[ FAILED \]") {
            $hasFailure = $true
        }
        
        # Also check exit code
        if ($exitCode -ne 0) {
            $hasFailure = $true
        }
        
        if ($hasFailure) {
            $failedCount++
            $failedTests += @{
                Name = $testName
                Output = $processOutput
                ExitCode = $exitCode
            }
            Write-Host "`n[FAILED] $testName (Exit Code: $exitCode)" -ForegroundColor Red
        } else {
            $passedTests++
            Write-Host "`n[PASSED] $testName" -ForegroundColor Green
        }
    }
    catch {
        $failedCount++
        $failedTests += @{
            Name = $testName
            Output = "Error running test: $_"
            ExitCode = -1
        }
        Write-Host "`n[ERROR] $testName - Exception: $_" -ForegroundColor Red
    }
}

# Summary
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "Test Summary" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Total:  $totalTests" -ForegroundColor White
Write-Host "Passed: $passedTests" -ForegroundColor Green
Write-Host "Failed: $failedCount" -ForegroundColor $(if ($failedCount -eq 0) { "Green" } else { "Red" })
Write-Host "========================================`n" -ForegroundColor Cyan

# Display failures if any
if ($failedTests.Count -gt 0) {
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "FAILED TESTS" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    
    foreach ($failed in $failedTests) {
        Write-Host "`n[$($failed.Name)]" -ForegroundColor Red
        Write-Host "Exit Code: $($failed.ExitCode)" -ForegroundColor Yellow
        Write-Host "----------------------------------------" -ForegroundColor DarkGray
        
        # Extract and display failure lines
        $failureLines = $failed.Output -split "`n" | Where-Object { $_ -match "\[ FAILED \]" -or $_ -match "FAILED" -or $_ -match "error" -or $_ -match "Error" }
        
        if ($failureLines.Count -gt 0) {
            Write-Host "Failure details:" -ForegroundColor Yellow
            foreach ($line in $failureLines) {
                Write-Host "  $line" -ForegroundColor Red
            }
        } else {
            # If no specific failure lines found, show last 20 lines
            $lastLines = ($failed.Output -split "`n")[-20..-1]
            Write-Host "Last output lines:" -ForegroundColor Yellow
            foreach ($line in $lastLines) {
                if ($line.Trim()) {
                    Write-Host "  $line" -ForegroundColor Gray
                }
            }
        }
    }
    
    Write-Host "`n========================================`n" -ForegroundColor Red
    exit 1
} else {
    Write-Host "All tests passed!`n" -ForegroundColor Green
    exit 0
}

