@echo off
setlocal enabledelayedexpansion

set "BUNDLED_CURL_ROOT=%~dp0curl-8.19.0_5-win64-mingw"
set "MINGW64_BIN=C:\msys64\mingw64\bin"
set "MINGW32_BIN=C:\mingw32\bin"

if "%~1"=="" (
  if exist "%MINGW64_BIN%\cmake.exe" (
    set "CMAKE_EXE=%MINGW64_BIN%\cmake.exe"
  ) else if exist "%MINGW32_BIN%\cmake.exe" (
    set "CMAKE_EXE=%MINGW32_BIN%\cmake.exe"
  ) else (
    set "CMAKE_EXE=cmake"
  )
) else (
  set "CMAKE_EXE=%~1"
)

if "%~2"=="" (
  set "LIBCURL_INCLUDE=%BUNDLED_CURL_ROOT%\include"
) else (
  set "LIBCURL_INCLUDE=%~2"
)

if "%~3"=="" (
  set "LIBCURL_LIBRARY=%BUNDLED_CURL_ROOT%\lib\libcurl.dll.a"
) else (
  set "LIBCURL_LIBRARY=%~3"
)

if "%~4"=="" (
  set "LIBCURL_DLL=%BUNDLED_CURL_ROOT%\bin\libcurl-x64.dll"
) else (
  set "LIBCURL_DLL=%~4"
)

set "BUILD_DIR=build"

if not exist "%LIBCURL_INCLUDE%\curl\curl.h" (
  echo Could not find curl headers at "%LIBCURL_INCLUDE%".
  echo Usage: build_mingw.bat ["C:\path\to\cmake.exe"] ["C:\path\to\curl\include"] ["C:\path\to\curl\lib\libcurl.dll.a"] ["C:\path\to\curl\bin\libcurl-x64.dll"]
  exit /b 1
)

if not exist "%LIBCURL_LIBRARY%" (
  echo Could not find libcurl import library at "%LIBCURL_LIBRARY%".
  exit /b 1
)

if exist "%MINGW64_BIN%\g++.exe" (
  set "CXX_COMPILER=%MINGW64_BIN%\g++.exe"
  set "TOOLCHAIN_BIN=%MINGW64_BIN%"
) else if exist "%MINGW32_BIN%\g++.exe" (
  set "CXX_COMPILER=%MINGW32_BIN%\g++.exe"
  set "TOOLCHAIN_BIN=%MINGW32_BIN%"
) else (
  set "CXX_COMPILER=g++"
  set "TOOLCHAIN_BIN="
)

if defined TOOLCHAIN_BIN (
  set "PATH=%TOOLCHAIN_BIN%;%PATH%"
)

if exist "%MINGW64_BIN%\mingw32-make.exe" (
  set "MAKE_PROGRAM=%MINGW64_BIN%\mingw32-make.exe"
 ) else if exist "%MINGW32_BIN%\mingw32-make.exe" (
  set "MAKE_PROGRAM=%MINGW32_BIN%\mingw32-make.exe"
) else (
  set "MAKE_PROGRAM=mingw32-make"
)

if exist "%CMAKE_EXE%" (
  rem CMake was found by absolute path.
) else (
  where.exe "%CMAKE_EXE%" >nul 2>nul
  if errorlevel 1 (
    echo Could not find CMake.
    echo Expected "%MINGW64_BIN%\cmake.exe", "%MINGW32_BIN%\cmake.exe", or cmake on PATH.
    echo In MSYS2 MinGW64, install it with:
    echo pacman -S mingw-w64-x86_64-cmake
    exit /b 1
  )
)

if exist "%CXX_COMPILER%" (
  rem Compiler was found by absolute path.
) else (
  where.exe "%CXX_COMPILER%" >nul 2>nul
  if errorlevel 1 (
    echo Could not find the MinGW64 C++ compiler.
    echo Expected "%MINGW64_BIN%\g++.exe", "%MINGW32_BIN%\g++.exe", or g++ on PATH.
    echo In MSYS2 MinGW64, install it with:
    echo pacman -S mingw-w64-x86_64-gcc
    exit /b 1
  )
)

if exist "%MAKE_PROGRAM%" (
  rem Make was found by absolute path.
) else (
  where.exe "%MAKE_PROGRAM%" >nul 2>nul
  if errorlevel 1 (
    echo Could not find mingw32-make.
    echo Expected "%MINGW64_BIN%\mingw32-make.exe", "%MINGW32_BIN%\mingw32-make.exe", or mingw32-make on PATH.
    echo In MSYS2 MinGW64, install it with:
    echo pacman -S mingw-w64-x86_64-make
    exit /b 1
  )
)

for /f "usebackq delims=" %%T in (`"%CXX_COMPILER%" -dumpmachine`) do set "CXX_TARGET=%%T"
echo Using compiler target: %CXX_TARGET%
if /i "%CXX_TARGET%"=="i686-w64-mingw32" (
  echo The compiler in "%MINGW32_BIN%" is 32-bit, but the bundled curl package is 64-bit.
  echo Use a 64-bit MinGW compiler, or replace curl-8.19.0_5-win64-mingw with a 32-bit curl package.
  echo For MSYS2 MinGW64, install the needed tools with:
  echo pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make
  exit /b 1
)

"%CMAKE_EXE%" -S . -B "%BUILD_DIR%" -G "MinGW Makefiles" ^
  -DCMAKE_CXX_COMPILER="%CXX_COMPILER%" ^
  -DCMAKE_MAKE_PROGRAM="%MAKE_PROGRAM%" ^
  -DLIBCURL_INCLUDE_DIR="%LIBCURL_INCLUDE%" ^
  -DLIBCURL_LIBRARY="%LIBCURL_LIBRARY%" ^
  -DLIBCURL_DLL="%LIBCURL_DLL%"
if errorlevel 1 exit /b %errorlevel%

"%CMAKE_EXE%" --build "%BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%

echo Build completed successfully.
