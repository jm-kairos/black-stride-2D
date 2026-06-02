REM Build script for engine
@ECHO OFF
SetLocal EnableDelayedExpansion

REM Get a list of all the .cpp files
SET cppFilenames =
FOR /R %%f in (*.cpp) do (
    SET cppFilenames=!cppFilenames! %%f
)

REM echo "Files:" %cppFilenames%

SET assembly=engine
SET compilerFlags=-g -shared -Wvarargs -Wall -Werror
REM -Wall -Werror
SET includeFlags=-Isource -Ivendor/include
SET linkerFlags=-Lvendor/lib -lSDL3
SET defines=-DBS_DEBUG -DBSEXPORT -D_CRT_SECURE_NO_WARNINGS

ECHO "Building %assembly%..."
clang++ %cppFilenames% %compilerFlags% -o ../bin/%assembly%.dll %defines% %includeFlags% %linkerFlags% 