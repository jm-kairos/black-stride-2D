@ECHO OFF
REM Builds the vendored FreeType submodule (engine\vendor\freetype) into the static lib
REM the engine link expects: engine\vendor\freetype\lib\freetype.lib.
REM Run once after cloning (git submodule update --init) or after bumping the submodule.
REM All optional external deps (zlib/bzip2/png/harfbuzz/brotli) are disabled so the lib
REM is self-contained. Uses cmake + ninja + clang from PATH.

SETLOCAL

SET ftDir=vendor\freetype
SET ftBuildDir=%ftDir%\build-clang
SET ftLibDir=%ftDir%\lib

IF NOT EXIST %ftDir%\include\ft2build.h (
    ECHO FreeType sources missing. Run: git submodule update --init engine/vendor/freetype
    EXIT /B 1
)

cmake -S %ftDir% -B %ftBuildDir% -G Ninja ^
    -DCMAKE_C_COMPILER=clang ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DBUILD_SHARED_LIBS=OFF ^
    -DFT_DISABLE_ZLIB=TRUE ^
    -DFT_DISABLE_BZIP2=TRUE ^
    -DFT_DISABLE_PNG=TRUE ^
    -DFT_DISABLE_HARFBUZZ=TRUE ^
    -DFT_DISABLE_BROTLI=TRUE
IF %ERRORLEVEL% NEQ 0 (ECHO FreeType configure failed && EXIT /B 1)

cmake --build %ftBuildDir%
IF %ERRORLEVEL% NEQ 0 (ECHO FreeType build failed && EXIT /B 1)

IF NOT EXIST %ftLibDir% MKDIR %ftLibDir%
COPY /Y %ftBuildDir%\freetype.lib %ftLibDir%\freetype.lib >nul
IF %ERRORLEVEL% NEQ 0 (ECHO FreeType lib copy failed && EXIT /B 1)

ECHO FreeType built: %ftLibDir%\freetype.lib
ENDLOCAL
