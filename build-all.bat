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

ECHO "All assemblies built successfully."