@echo off
setlocal

REM === Setup test directory ===
set TEST_DIR=test_folder

echo Cleaning old test folder...
rmdir /s /q %TEST_DIR% 2>nul

echo Creating test structure...
mkdir %TEST_DIR%

REM === Create test files ===
echo dummy > %TEST_DIR%\file1.txt
echo dummy > %TEST_DIR%\file2.pdf
echo dummy > %TEST_DIR%\image1.jpg
echo dummy > %TEST_DIR%\image2.png
echo dummy > %TEST_DIR%\video1.mp4
echo dummy > %TEST_DIR%\video2.mkv
echo dummy > %TEST_DIR%\random.xyz