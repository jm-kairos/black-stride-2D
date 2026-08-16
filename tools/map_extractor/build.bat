@ECHO OFF
SetLocal EnableDelayedExpansion

REM Build the 4-Map Extractor tool.
REM Uses the same SDL3 / Dear ImGui dependency stack as the engine.

SET ROOT=%~dp0\..\..
SET SRC=%~dp0
SET OUT=%ROOT%\bin\map_extractor.exe

SET IMGUI_DIR=%ROOT%\engine\vendor\imgui
SET VENDOR_INCLUDE=%ROOT%\engine\vendor\include
SET VENDOR_LIB=%ROOT%\engine\vendor\lib
SET IMGUI_OBJ_DIR=%ROOT%\engine\obj\imgui

REM Compile ImGui backend objects if missing (mirror engine build).
IF NOT EXIST %IMGUI_OBJ_DIR% MKDIR %IMGUI_OBJ_DIR%
SET IMGUI_OBJS=
FOR %%f IN (
    %IMGUI_DIR%\imgui.cpp
    %IMGUI_DIR%\imgui_draw.cpp
    %IMGUI_DIR%\imgui_tables.cpp
    %IMGUI_DIR%\imgui_widgets.cpp
    %IMGUI_DIR%\imgui_demo.cpp
    %IMGUI_DIR%\backends\imgui_impl_sdl3.cpp
    %IMGUI_DIR%\backends\imgui_impl_sdlgpu3.cpp
) DO (
    SET objPath=%IMGUI_OBJ_DIR%\%%~nf.o
    SET IMGUI_OBJS=!IMGUI_OBJS! !objPath!
    IF NOT EXIST !objPath! (
        ECHO "Compiling ImGui: %%~nxf"
        clang++ -g -c %%f -o !objPath! -w -I%IMGUI_DIR% -I%IMGUI_DIR%\backends -I%VENDOR_INCLUDE% -D_CRT_SECURE_NO_WARNINGS
        IF !ERRORLEVEL! NEQ 0 (echo ImGui compile failed: %%~nxf && exit /b !ERRORLEVEL!)
    )
)

SET compilerFlags=-g -Wall -Werror
SET includeFlags=-I%VENDOR_INCLUDE% -I%IMGUI_DIR% -I%IMGUI_DIR%\backends -I%SRC%
SET linkerFlags=-L%VENDOR_LIB% -lSDL3 -luser32 -lshell32 -lcomdlg32 -lole32
SET defines=-D_CRT_SECURE_NO_WARNINGS

ECHO "Building map_extractor..."
clang++ %SRC%\extractor.cpp %SRC%\preview.cpp %SRC%\main.cpp %IMGUI_OBJS% %compilerFlags% -o %OUT% %defines% %includeFlags% %linkerFlags%
