@ECHO OFF
REM Build Everything

ECHO "Building everything..."

REM Attemps to build engine first
REM In case of failure, echo the error level and exit the build 

PUSHD engine
CALL build.bat
POPD
IF %ERRORLEVEL% NEQ 0 (echo Error:%ERRORLEVEL% && exit)

PUSHD sandbox
CALL build.bat
POPD 
IF %ERRORLEVEL% NEQ 0 (echo Error:%ERRORLEVEL% && exit)

REM Compile any edited shaders (incremental) so DXIL/SPIR-V blobs are current before staging.
REM Self-contained pure-batch compiler (no external scripts). Auto-discovers every *.hlsl
REM under assets\shaders\src named <name>.<stage>.hlsl and emits BOTH targets:
REM   DXIL  -> assets\shaders\dxil\<name>.<stage>.dxil   (d3d12 backend)
REM   SPIR-V -> assets\shaders\spirv\<name>.<stage>.spv  (vulkan backend)
REM Uses dxc from PATH or the Vulkan SDK; if dxc is unavailable, skip and keep existing blobs.
SET "DXC="
where dxc >nul 2>nul && SET "DXC=dxc"
IF NOT DEFINED DXC IF DEFINED VULKAN_SDK IF EXIST "%VULKAN_SDK%\Bin\dxc.exe" SET "DXC=%VULKAN_SDK%\Bin\dxc.exe"
IF NOT DEFINED DXC (
    ECHO "Skipping shader compile (dxc not found; using existing blobs)."
    GOTO :after_shaders
)

ECHO "Compiling shaders (incremental)..."
IF NOT EXIST "assets\shaders\dxil"  MKDIR "assets\shaders\dxil"
IF NOT EXIST "assets\shaders\spirv" MKDIR "assets\shaders\spirv"
SET "SH_COMPILED=0"
SET "SH_SKIPPED=0"
SET "SH_FAILED=0"
FOR %%F IN (assets\shaders\src\*.hlsl) DO CALL :compile_one "%%F"
ECHO Compiled %SH_COMPILED% shader(s); %SH_SKIPPED% up-to-date; %SH_FAILED% failure(s).
IF %SH_FAILED% NEQ 0 (ECHO Shader compile error && EXIT /B 1)
:after_shaders

REM Stage runtime assets next to the executable. The game loads data files (e.g.
REM assets/ship_deck.ship) relative to its working directory (bin/), so the authoritative
REM assets/ tree at the repo root must be mirrored into bin/ on every build. Without this,
REM edits to assets/ never reach the running game. /E recurses (incl. empty dirs), /Y
REM overwrites without prompting, /I assumes the destination is a directory.
IF EXIST assets (
    ECHO "Staging assets..."
    XCOPY assets bin\assets /E /Y /I >nul
)

ECHO "Engine and sandbox built successfully."
GOTO :eof


REM ==========================================================================
REM Subroutines
REM ==========================================================================

REM Compile a single shader source. %1 = full path to <name>.<stage>.hlsl
:compile_one
SET "SRC=%~1"
SET "BASE=%~n1"
FOR %%B IN ("%BASE%") DO (
    SET "NAME=%%~nB"
    SET "STG=%%~xB"
)
IF NOT DEFINED STG (
    ECHO   SKIP %~nx1: expected ^<name^>.^<stage^>.hlsl
    GOTO :eof
)
SET "STG=%STG:~1%"
SET "PROFILE="
IF /I "%STG%"=="vert" SET "PROFILE=vs_6_0"
IF /I "%STG%"=="frag" SET "PROFILE=ps_6_0"
IF /I "%STG%"=="comp" SET "PROFILE=cs_6_0"
IF NOT DEFINED PROFILE (
    ECHO   SKIP %~nx1: unknown stage "%STG%"
    GOTO :eof
)
SET "DXILF=assets\shaders\dxil\%NAME%.%STG%.dxil"
SET "SPVF=assets\shaders\spirv\%NAME%.%STG%.spv"

REM Incremental: skip when both blobs exist and neither is older than the source.
CALL :is_uptodate "%SRC%" "%DXILF%" "%SPVF%"
IF NOT ERRORLEVEL 1 (
    SET /A SH_SKIPPED+=1
    GOTO :eof
)

ECHO   [%NAME%.%STG%] -^> DXIL + SPIR-V
"%DXC%" -T %PROFILE% -E main "%SRC%" -Fo "%DXILF%"
IF ERRORLEVEL 1 (ECHO     DXIL FAILED: %NAME%.%STG% && SET /A SH_FAILED+=1 && GOTO :eof)
"%DXC%" -T %PROFILE% -E main -spirv -fspv-target-env=vulkan1.2 "%SRC%" -Fo "%SPVF%"
IF ERRORLEVEL 1 (ECHO     SPIR-V FAILED: %NAME%.%STG% && SET /A SH_FAILED+=1 && GOTO :eof)
SET /A SH_COMPILED+=1
GOTO :eof

REM Return errorlevel 0 when the compiled blobs are up-to-date (skip), 1 when a
REM (re)compile is needed. %1=source .hlsl  %2=dxil blob  %3=spirv blob.
REM XCOPY /D /L lists the source only when it is newer than the destination file;
REM the destination already exists so XCOPY treats it as a file (no prompt).
:is_uptodate
IF NOT EXIST "%~2" EXIT /B 1
IF NOT EXIST "%~3" EXIT /B 1
FOR /F "delims=" %%X IN ('XCOPY /D /Y /L "%~1" "%~2" 2^>nul ^| FIND /I ".hlsl"') DO EXIT /B 1
FOR /F "delims=" %%X IN ('XCOPY /D /Y /L "%~1" "%~3" 2^>nul ^| FIND /I ".hlsl"') DO EXIT /B 1
EXIT /B 0