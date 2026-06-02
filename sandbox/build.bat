REM Build script for sandbox
@ECHO OFF
SetLocal EnableDelayedExpansion

REM Get a list of all the .cpp files
SET cppFilenames =
FOR /R %%f in (*.cpp) do (
    SET cppFilenames=!cppFilenames! %%f
)

REM echo "Files:" %cppFilenames%

SET assembly=sandbox
SET compilerFlags=-g
REM -Wall -Werror
SET includeFlags=-Isource -I../engine/source/
SET linkerFlags=-L../bin/ -lengine.lib
SET defines=-DBS_DEBUG -DBSIMPORT

ECHO "Building %assembly%..."
clang++ %cppFilenames% %compilerFlags% -o ../bin/%assembly%.exe %defines% %includeFlags% %linkerFlags%
IF %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

REM Copy SDL3.dll next to the executable so it can be found at runtime.
IF EXIST ..\engine\vendor\bin\SDL3.dll (
    COPY /Y ..\engine\vendor\bin\SDL3.dll ..\bin\SDL3.dll >nul
)
