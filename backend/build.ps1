# build.ps1 - Build script for the Download Manager C backend
# Usage: .\build.ps1

Write-Host "Building Download Manager..." -ForegroundColor Cyan

$src = "src\main.c", "src\logger.c", "src\file_manager.c", "src\download_engine.c", "src\download_queue.c", "src\history.c", "src\api_server.c", "src\cjson\cJSON.c"
$out = "download_manager.exe"
$flags = "-Wall", "-Wextra", "-O2", "-DCJSON_HIDE_SYMBOLS"
$libs = "-lwininet", "-lws2_32"

$cmd = "gcc $($flags -join ' ') $($src -join ' ') -o $out $($libs -join ' ')"
Write-Host "Command: $cmd" -ForegroundColor Gray

& gcc @flags @src -o $out @libs 2>&1

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nBuild successful: $out" -ForegroundColor Green
    Write-Host "Run with: .\download_manager.exe" -ForegroundColor Yellow
} else {
    Write-Host "`nBuild FAILED" -ForegroundColor Red
    exit 1
}
