@echo off
setlocal

for %%I in (
"%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
"%ProgramFiles%\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
"%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
"%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
) do if exist %%I set "MSBUILD=%%~I"

if not defined MSBUILD (
  where msbuild >nul 2>nul
  if not errorlevel 1 set "MSBUILD=msbuild"
)

if not defined MSBUILD (
  echo MSBuild was not found.
  echo Install Visual Studio 2022 Build Tools with Desktop development with C++.
  pause
  exit /b 1
)

"%MSBUILD%" NvidiaMemoryWatchdog.sln /t:Rebuild /m /p:Configuration=Release /p:Platform=x64
if errorlevel 1 (
  echo.
  echo Build FAILED.
  pause
  exit /b 1
)

if not exist x64\Release\NvidiaMemoryWatchdog.exe (
  echo.
  echo Build reported success but the EXE was not found.
  pause
  exit /b 1
)

echo.
echo Build succeeded.
echo EXE: x64\Release\NvidiaMemoryWatchdog.exe
pause
endlocal
