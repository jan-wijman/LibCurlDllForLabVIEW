curl_lv_wrapper
================

This project builds a Windows DLL wrapper around libcurl for LabVIEW using MinGW-w64 and CMake.
The wrapper exports a C-compatible ABI and provides synchronous HTTP methods plus true asynchronous GET/POST support.

Supported features
------------------
- C++17 with MinGW-w64 g++
- Windows shared library (`curl_lv.dll`)
- `extern "C"` exports with `__cdecl` calling convention
- Stable integer error codes and caller-supplied buffers
- No C++/STL or libcurl types in the public API
- Generic sync request plus convenience wrappers
- Async GET and POST with worker threads and chunk polling
- Safe libcurl global init/cleanup

Build instructions
------------------
The project is configured to use the bundled curl package in:

curl-8.19.0_5-win64-mingw

Open a MinGW-w64 shell and run:

build_mingw.bat

If CMake is not on your PATH, pass the CMake executable:

build_mingw.bat "C:/path/to/cmake.exe"

You can also run CMake directly:

cmake -S . -B build -G "MinGW Makefiles" ^
  -DCMAKE_CXX_COMPILER=g++

cmake --build build

To use a different curl package, pass explicit paths:

build_mingw.bat "C:/path/to/cmake.exe" "C:/path/to/curl/include" "C:/path/to/curl/lib/libcurl.dll.a" "C:/path/to/curl/bin/libcurl-x64.dll"

If you use `LIBCURL_ROOT`, you may also set it instead of the individual include/library paths.

Dependency setup
----------------
- Install MinGW-w64 with a 64-bit toolchain if you need 64-bit LabVIEW compatibility.
- Install a libcurl build for MinGW-w64. The package should provide:
  - `curl/curl.h`
  - `libcurl.dll.a` (import library)
  - `libcurl-x64.dll` or `libcurl.dll`

DLL deployment notes
--------------------
Copy the following files beside `curl_lv.dll` for deployment:
- `libcurl-x64.dll` or `libcurl.dll` (if `LIBCURL_DLL` was not deployed automatically)
- `libgcc_s_seh-1.dll`
- `libstdc++-6.dll`
- `libwinpthread-1.dll`

The exact runtime DLLs depend on your MinGW-w64 distribution and link options.

LabVIEW notes
-------------
- Build a 64-bit DLL for 64-bit LabVIEW.
- Use the LabVIEW "Import Shared Library" wizard.
- The wrapper functions use `__cdecl` convention.
- All string outputs require caller-managed buffers plus buffer sizes.
- Validate and size buffers before calling.

Async polling usage
-------------------
1. Call `lv_curl_global_init()` once.
2. Start an async request with `lv_curl_async_get_start()` or `lv_curl_async_post_start()`.
3. Poll `lv_curl_async_read_chunk()` repeatedly until it returns `LV_CURL_ERROR_ASYNC_COMPLETE`.
4. Optionally check state with `lv_curl_async_get_state()`.
5. Call `lv_curl_async_cancel()` to abort a running request.
6. Call `lv_curl_global_cleanup()` when the app shuts down.

Project files
-------------
- `include/curl_lv.h` : Public LabVIEW-compatible API definitions.
- `src/curl_lv.cpp` : Synchronous request implementation and global init/cleanup.
- `src/curl_lv_async.cpp` : Asynchronous request management and chunk polling.
- `src/curl_lv_internal.hpp` : Internal helpers and shared async state.
- `examples/labview_usage.txt` : LabVIEW import wizard settings and example usage.
