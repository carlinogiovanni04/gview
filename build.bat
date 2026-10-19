@echo off
setlocal
pushd "%~dp0"

if not defined CC set CC=clang-cl
if not defined SDL_DIR set SDL_DIR=C:\Users\Carli\Code\_libraries_
if not defined install set install=C:\Users\Carli\Software

set INCLUDES=/I "include" /I "..\gcui\include" /I "%SDL_DIR%\include"
set LIBS="%SDL_DIR%\lib\x64\SDL3.lib" "%SDL_DIR%\lib\x64\SDL3_ttf.lib"

if not exist bin mkdir bin

%CC% /nologo /std:c11 /W4 /Zi /Od %INCLUDES% "src\main.c" /Fe:"bin\gview.exe" /Fo:"bin\\" /link /DEBUG %LIBS%
if errorlevel 1 (
    echo Build failed: gview.
    popd
    exit /b 1
)

copy /Y "%SDL_DIR%\bin\SDL3.dll" "bin\" >nul
copy /Y "%SDL_DIR%\bin\SDL3_ttf.dll" "bin\" >nul
copy /Y bin\gview.exe "%install%" >nul

echo Built bin\gview.exe
popd
endlocal
