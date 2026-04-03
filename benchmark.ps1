# Clean previous test folder
$testDir = "test_folder"
if (Test-Path $testDir) {
    Remove-Item $testDir -Recurse -Force
}

# Extensions pool (realistic mix)
$extensions = @(
    ".txt", ".pdf", ".md", ".docx", ".xlsx",     # Documents
    ".jpg", ".png", ".jpeg", ".gif",             # Images
    ".mp4", ".mkv", ".avi",                      # Videos
    ".cpp", ".py", ".js",                        # Code / Other
    ".xyz", ".dat", ".bin"                       # Unknown
)

Write-Host "Creating realistic test structure..."

New-Item -ItemType Directory -Path $testDir | Out-Null

1..10 | ForEach-Object {
    $subDir = "$testDir\sub$_"
    New-Item -ItemType Directory -Path $subDir | Out-Null

    1..100 | ForEach-Object {
        # Pick random extension
        $ext = $extensions | Get-Random

        # Occasionally create duplicate names (to test rename logic)
        if ((Get-Random -Minimum 1 -Maximum 10) -eq 1) {
            $fileName = "duplicate$ext"
        } else {
            $fileName = "file_$($_)$ext"
        }

        New-Item "$subDir\$fileName" -ItemType File | Out-Null
    }
}

Write-Host "Test structure created."

# Run organizer with timing
Write-Host "Running organizer (recursive)..."

$time = Measure-Command {
    .\organizer.exe $testDir --recursive
}

# Output results
Write-Host "`n=== RESULT ==="
Write-Host "Total time: $($time.TotalMilliseconds) ms"
Write-Host "Seconds: $($time.TotalSeconds)"