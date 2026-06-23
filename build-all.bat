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

REM Stage runtime assets next to the executable. The game loads data files (e.g.
REM assets/ship_deck.ship) relative to its working directory (bin/), so the authoritative
REM assets/ tree at the repo root must be mirrored into bin/ on every build. Without this,
REM edits to assets/ never reach the running game. /E recurses (incl. empty dirs), /Y
REM overwrites without prompting, /I assumes the destination is a directory.
IF EXIST assets (
    ECHO "Staging assets..."
    XCOPY assets bin\assets /E /Y /I >nul
)

ECHO "All assemblies built successfully."