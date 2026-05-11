@echo off
echo Configuring CMake project...
if not exist build mkdir build
cd build
cmake ..
if %errorlevel% neq 0 (
    echo CMake configuration failed!
    cd ..
    pause
    exit /b %errorlevel%
)

echo.
echo Building Release executable...
cmake --build . --config Release
if %errorlevel% neq 0 (
    echo Build failed!
    cd ..
    pause
    exit /b %errorlevel%
)

echo.
echo =======================================================
echo Build successful! 
echo Executable is located at: build\Release\DeCrusher.exe
echo =======================================================
cd ..
pause
