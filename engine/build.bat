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
REM Vendored RmlUi (third-party) — compiled SEPARATELY with relaxed warnings, cached, then
REM archived into a single static lib.
REM Same rationale as ImGui: RmlUi is not our code and will not survive -Wall -Werror. Core +
REM Debugger + the default (FreeType) font engine are compiled once into obj\rmlui and reused;
REM delete obj\rmlui to force a clean rebuild. RMLUI_STATIC_LIB keeps RmlUi's symbols internal to
REM engine.dll (only the bs_rml facade is exported to the game). FreeType is linked from its
REM prebuilt static lib (vendor\freetype\lib\freetype.lib, built once by build_freetype.bat).
REM Object names encode the path below Source\ (e.g. Core_Geometry.o) so the two Geometry.cpp
REM files never collide.
REM
REM The ~199 objects are archived into obj\rmlui\rmlui.lib and the final link references that ONE
REM path instead of listing every object: cmd.exe caps a command line (and any SET) at 8191 chars,
REM which listing all objects blows past. The archive is rebuilt only when an object is (re)compiled
REM or the lib is missing, so steady-state incremental builds skip it entirely. To stay under the
REM command-line cap AND avoid a slow O(n^2) rebuild, object paths are written one-per-line into a
REM response file (short "ECHO >>" appends) and archived in a SINGLE "llvm-ar rcs @rsp" call.
REM =====================================================================================
SET rmluiDir=vendor\rmlui
SET rmluiObjDir=obj\rmlui
SET rmluiLib=%rmluiObjDir%\rmlui.lib
SET rmluiRsp=%rmluiObjDir%\rmlui.rsp
SET rmluiInclude=-I%rmluiDir%\Include -Ivendor\freetype\include
SET rmluiDefs=-DRMLUI_STATIC_LIB -DRMLUI_FONT_ENGINE_FREETYPE -D_CRT_SECURE_NO_WARNINGS

IF NOT EXIST %rmluiObjDir% MKDIR %rmluiObjDir%

SET rmluiNeedsArchive=0
IF NOT EXIST %rmluiLib% SET rmluiNeedsArchive=1

FOR /R %rmluiDir%\Source %%f in (*.cpp) DO (
    SET rel=%%f
    SET rel=!rel:*Source\=!
    SET objName=!rel:\=_!
    SET objName=!objName:.cpp=!
    SET objPath=%rmluiObjDir%\!objName!.o
    IF NOT EXIST !objPath! (
        ECHO "Compiling RmlUi: !rel!"
        clang++ -g -std=c++17 -c %%f -o !objPath! -w %rmluiInclude% %rmluiDefs%
        IF !ERRORLEVEL! NEQ 0 (echo RmlUi compile failed: !rel! && exit /b !ERRORLEVEL!)
        SET rmluiNeedsArchive=1
    )
)

IF "!rmluiNeedsArchive!"=="1" (
    ECHO "Archiving RmlUi -> %rmluiLib%"
    IF EXIST %rmluiRsp% DEL %rmluiRsp%
    FOR /R %rmluiDir%\Source %%f in (*.cpp) DO (
        SET rel=%%f
        SET rel=!rel:*Source\=!
        SET objName=!rel:\=_!
        SET objName=!objName:.cpp=!
        ECHO %rmluiObjDir%\!objName!.o>>%rmluiRsp%
    )
    IF EXIST %rmluiLib% DEL %rmluiLib%
    llvm-ar rcs %rmluiLib% @%rmluiRsp%
    IF !ERRORLEVEL! NEQ 0 (echo RmlUi archive failed && exit /b !ERRORLEVEL!)
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
SET includeFlags=-Isource -Ivendor/include -isystem %imguiDir% -isystem %imguiDir%\backends -isystem vendor\rmlui\Include -isystem vendor\freetype\include
SET linkerFlags=-Lvendor/lib -lSDL3 -Lvendor\freetype\lib -lfreetype
SET defines=-DBS_DEBUG -DBSEXPORT -D_CRT_SECURE_NO_WARNINGS -DIMGUI_API="__declspec(dllexport)" -DRMLUI_STATIC_LIB

ECHO "Building %assembly%..."
clang++ %cppFilenames% %imguiObjs% %rmluiLib% %compilerFlags% -o ../bin/%assembly%.dll %defines% %includeFlags% %linkerFlags%
