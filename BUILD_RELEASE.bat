@echo off
setlocal

set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
    if not defined MSBUILD set "MSBUILD=%%I"
  )
)

if not defined MSBUILD (
  where msbuild >nul 2>nul
  if not errorlevel 1 set "MSBUILD=msbuild"
)

if not defined MSBUILD (
  echo MSBuild was not found.
  echo Install Visual Studio with Desktop development with C++.
  pause
  exit /b 1
)

"%MSBUILD%" NvidiaPowerStateWatchdog.sln /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
if errorlevel 1 (
  echo.
  echo Build FAILED.
  pause
  exit /b 1
)

if not exist x64\Release\NvidiaPowerStateWatchdog.exe (
  echo.
  echo Build reported success but the EXE was not found.
  pause
  exit /b 1
)

echo.
echo Build succeeded.
echo EXE: x64\Release\NvidiaPowerStateWatchdog.exe
pause
endlocal
