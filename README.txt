curl_lv_wrapper
================

This project builds a Windows DLL wrapper around libcurl for LabVIEW using MinGW-w64 and CMake.
The wrapper exports a C-compatible ABI and provides synchronous HTTP methods plus true asynchronous GET/POST support.

Supported features
------------------
- C++17 with MinGW-w64 g++
- Windows shared library (`curl_lv.dll`)
- `extern "C"` exports with `__cdecl` calling convention
- Stable integer error codes and caller-supplied output buffers
- Shared lossy diagnostics buffer readable with `lv_curl_get_latest_errors_info()`
- No C++/STL or libcurl types in the public API
- Open/close reference lifecycle for URL and authentication reuse
- Generic sync request plus convenience wrappers
- Sync multipart/form-data POST with text fields and file uploads
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
- HTTP response and async chunk buffers are binary-safe. Use the returned `actual_*_size` value as the byte count.
- Function calls return stable integer error codes. Detailed diagnostic text is collected in an internal lossy queue.
- Call `lv_curl_get_latest_errors_info()` to read and flush queued diagnostic text.
- `lv_curl_get_header_templates()` returns common HTTP header templates for LabVIEW UI lists or examples.

Async polling usage
-------------------
1. Call `lv_curl_global_init()` once.
2. Call `lv_curl_open()` with the base URL, default headers, and authentication info to get an I32 reference.
3. Start an async request with `lv_curl_async_get_start(reference, endpoint, optional_headers, ...)` or `lv_curl_async_post_start(reference, endpoint, optional_headers, ...)`.
4. Poll `lv_curl_async_read_chunk()` repeatedly until it returns `LV_CURL_ERROR_ASYNC_COMPLETE`.
5. Optionally check state with `lv_curl_async_get_state()`.
6. Call `lv_curl_async_cancel()` to abort a running request.
7. Call `lv_curl_close(reference)` when the URL/auth context is no longer needed.
8. Call `lv_curl_global_cleanup()` when the app shuts down.

Reference lifecycle
-------------------
`lv_curl_open()` stores:
- base URL
- default headers, one per line
- username/password for basic or negotiated libcurl authentication
- bearer token, added as `Authorization: Bearer <token>`

All sync calls and async start calls take the returned I32 reference, an endpoint, plus optional request headers.
Endpoints like `"items"` and `"/items"` are appended to the base URL.
An empty endpoint uses the base URL as-is, and a full `http://` or `https://` endpoint overrides the base URL for that request.
The request headers are appended after the default headers stored by `lv_curl_open()`.
Pass an empty string when a specific request does not need extra headers.
`lv_curl_get_header_templates()` returns common header templates; replace placeholder values before sending them.
`lv_curl_close()` removes the reference. Closing fails with `LV_CURL_ERROR_BUSY` while an async request for that reference is still running.

Multipart POST
--------------
`lv_curl_post_multipart()` sends a synchronous `multipart/form-data` POST.
Pass text fields as one `name=value` entry per line in `text_fields`.
Pass file fields as one `name=path` entry per line in `file_fields`.
Either string may be empty, but at least one text or file field is required.

Example:

text_fields:
description=Test upload
category=logs

file_fields:
file=C:\temp\report.txt
image=C:\temp\screenshot.png

Do not add a manual `Content-Type: multipart/form-data` header for this call.
Libcurl generates the multipart boundary automatically.

Binary response data
--------------------
Synchronous response buffers and asynchronous chunk buffers may contain binary data, including embedded NUL bytes.
The DLL copies exactly the received bytes and writes a trailing NUL only when the caller buffer has spare capacity.
Always use `actual_response_size` or `actual_chunk_size` as the number of valid bytes.
If the buffer is exactly the same size as the payload, the call succeeds without adding a trailing NUL.
For async chunks, `LV_CURL_ERROR_BUFFER_TOO_SMALL` leaves the chunk queued so the caller can retry with a larger buffer.

Diagnostics
-----------
Detailed error information is stored in a process-wide lossy queue with about 8 KB of text capacity.
The queue keeps the newest messages and drops oldest text when full.
Call `lv_curl_get_latest_errors_info(errors_buffer, errors_buffer_size, actual_errors_size)` after a non-success return code to copy the queued text.
The call flushes the queue only when the caller buffer is large enough.
If it returns `LV_CURL_ERROR_BUFFER_TOO_SMALL`, allocate a larger buffer using `actual_errors_size` and call again.

Project files
-------------
- `include/curl_lv.h` : Public LabVIEW-compatible API definitions.
- `src/curl_lv.cpp` : Synchronous request implementation and global init/cleanup.
- `src/curl_lv_async.cpp` : Asynchronous request management and chunk polling.
- `src/curl_lv_internal.hpp` : Internal helpers and shared async state.
- `examples/labview_usage.txt` : LabVIEW import wizard settings and example usage.
