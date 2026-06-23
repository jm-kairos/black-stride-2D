REM Build script for RTS controls unit tests.
@ECHO OFF
SetLocal EnableDelayedExpansion

SET assembly=test_rts_controls
SET compilerFlags=-g
SET includeFlags=-Isource -I../engine/source/
SET linkerFlags=-L../bin/ -lengine.lib
SET defines=-DBS_DEBUG -DBSIMPORT

ECHO "Building %assembly%..."
clang++ -x c++ source\test_rts_controls.cpp.off %compilerFlags% -o ../bin/%assembly%.exe %defines% %includeFlags%
IF %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

REM Copy SDL3.dll next to the executable so it can be found at runtime.
IF EXIST ..\engine\vendor\bin\SDL3.dll (
    COPY /Y ..\engine\vendor\bin\SDL3.dll ..\bin\SDL3.dll >nul
)

ECHO "Running %assembly%..."
..\bin\%assembly%.exe
