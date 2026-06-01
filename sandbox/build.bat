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
SET defines=-DIBX_DEBUG -DIBXIMPORT

ECHO "Building %assembly%..."
clang++ %cppFilenames% %compilerFlags% -o ../bin/%assembly%.exe %defines% %includeFlags% %linkerFlags% 