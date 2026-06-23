REM Build script for engine
@ECHO OFF
SetLocal EnableDelayedExpansion

REM =====================================================================================
REM Vendored Dear ImGui (third-party) — compiled SEPARATELY with relaxed warnings, cached.
REM
REM ImGui core + the SDL3 / SDL_GPU backends must NOT pass through the engine's strict
REM -Wall -Werror glob below (they are not our code and will not survive -Werror). We
REM compile them once into objects under obj\imgui and reuse them on later builds; the
REM final engine link pulls those objects in. Delete obj\imgui to force a clean rebuild.
REM =====================================================================================
SET imguiDir=vendor\imgui
SET imguiObjDir=obj\imgui
SET imguiCompileInclude=-I%imguiDir% -I%imguiDir%\backends -Ivendor/include
SET imguiObjs=

IF NOT EXIST %imguiObjDir% MKDIR %imguiObjDir%

FOR %%f IN (
    %imguiDir%\imgui.cpp
    %imguiDir%\imgui_draw.cpp
    %imguiDir%\imgui_tables.cpp
    %imguiDir%\imgui_widgets.cpp
    %imguiDir%\imgui_demo.cpp
    %imguiDir%\backends\imgui_impl_sdl3.cpp
    %imguiDir%\backends\imgui_impl_sdlgpu3.cpp
) DO (
    SET objPath=%imguiObjDir%\%%~nf.o
    SET imguiObjs=!imguiObjs! !objPath!
    IF NOT EXIST !objPath! (
        ECHO "Compiling ImGui: %%~nxf"
        clang++ -g -c %%f -o !objPath! -w %imguiCompileInclude% -DBS_DEBUG -D_CRT_SECURE_NO_WARNINGS -DIMGUI_API="__declspec(dllexport)"
        IF !ERRORLEVEL! NEQ 0 (echo ImGui compile failed: %%~nxf && exit /b !ERRORLEVEL!)
    )
)

REM =====================================================================================
REM Engine translation units. Scope the recursive glob to source\ so it never sweeps the
REM vendored ImGui tree under vendor\. ImGui headers are exposed to engine TUs via -isystem
REM so the backend TU can include them WITHOUT their warnings tripping -Werror.
REM =====================================================================================
SET cppFilenames=
FOR /R source %%f in (*.cpp) do (
    SET cppFilenames=!cppFilenames! %%f
)

REM echo "Files:" %cppFilenames%

SET assembly=engine
SET compilerFlags=-g -shared -Wvarargs -Wall -Werror
REM -Wall -Werror
SET includeFlags=-Isource -Ivendor/include -isystem %imguiDir% -isystem %imguiDir%\backends
SET linkerFlags=-Lvendor/lib -lSDL3
SET defines=-DBS_DEBUG -DBSEXPORT -D_CRT_SECURE_NO_WARNINGS -DIMGUI_API="__declspec(dllexport)"

ECHO "Building %assembly%..."
clang++ %cppFilenames% %imguiObjs% %compilerFlags% -o ../bin/%assembly%.dll %defines% %includeFlags% %linkerFlags%
