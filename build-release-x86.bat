@echo off
rem Build ezorsia (ijl15 proxy DLL) as Release|Win32 with MSVC v142.
rem ASCII only. Logs to build.log next to this file.
setlocal
cd /d "%~dp0"

set VSDIR=D:\VSBuildTools\BuildTools
set VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars32.bat
set MSB=%VSDIR%\MSBuild\Current\Bin\MSBuild.exe

if not exist "%VCVARS%" ( echo VCVARS_MISSING: %VCVARS% & goto :end )
if not exist "%MSB%"    ( echo MSBUILD_MISSING: %MSB% & goto :end )

echo ==== vcvars32 ====
call "%VCVARS%"
if errorlevel 1 ( echo VCVARS_FAILED & goto :end )

echo ==== build ====
rem Delete the stale artifact first. "if exist" alone reports an OLD dll as a
rem success after a failed build, which is how the 23:08 C2488 error slipped by.
if exist "%~dp0out\Release\ijl15.dll" del /q "%~dp0out\Release\ijl15.dll"

"%MSB%" ezorsia.sln /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v142 /v:m /nologo > "%~dp0build.log" 2>&1
set RC=%errorlevel%
echo EXITCODE=%RC%

if not "%RC%"=="0" (
  echo BUILD_FAILED - compile/link did not succeed
  echo ---------------- build.log ----------------
  type "%~dp0build.log"
  echo -----------------------------------------
  goto :end
)

rem OutDir is $(SolutionDir)\out\$(Configuration)\  (see ezorsia.vcxproj:83/91)
rem (no findstr here: it reports a write error on some redirected logs)
if exist "%~dp0out\Release\ijl15.dll" (
  echo DLL_OK
  for %%F in ("%~dp0out\Release\ijl15.dll") do echo   %%~nxF  %%~zF bytes  %%~tF
) else (
  echo DLL_MISSING
)

:end
echo.
echo Log: %~dp0build.log
echo Press any key to close.
pause >nul
endlocal
