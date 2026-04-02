@echo off
setlocal
REM === Cleanup test directory ===
set TEST_DIR=test_folder
echo Cleaning test folder...
rmdir /s /q %TEST_DIR% 2>nul