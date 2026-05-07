#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Stable error codes
#define LV_CURL_SUCCESS 0
#define LV_CURL_ERROR_INVALID_ARGUMENT 1
#define LV_CURL_ERROR_INITIALIZATION 2
#define LV_CURL_ERROR_CURL 3
#define LV_CURL_ERROR_BUFFER_TOO_SMALL 4
#define LV_CURL_ERROR_NOT_FOUND 5
#define LV_CURL_ERROR_BUSY 6
#define LV_CURL_ERROR_CANCELLED 7
#define LV_CURL_ERROR_INTERNAL 8
#define LV_CURL_ERROR_ASYNC_NOT_FOUND 9
#define LV_CURL_ERROR_ASYNC_COMPLETE 10
#define LV_CURL_ERROR_ASYNC_NO_CHUNK 11
#define LV_CURL_ERROR_ASYNC_FAILED 12

// HTTP method values for lv_curl_request
#define LV_CURL_METHOD_GET 0
#define LV_CURL_METHOD_POST 1
#define LV_CURL_METHOD_PUT 2
#define LV_CURL_METHOD_PATCH 3
#define LV_CURL_METHOD_DELETE 4
#define LV_CURL_METHOD_HEAD 5
#define LV_CURL_METHOD_OPTIONS 6

// Async request state values
#define LV_CURL_ASYNC_STATE_RUNNING 1
#define LV_CURL_ASYNC_STATE_COMPLETED 2
#define LV_CURL_ASYNC_STATE_CANCELLED 3
#define LV_CURL_ASYNC_STATE_FAILED 4

#define LV_CURL_API extern "C" __declspec(dllexport)
#define LV_CURL_CALL __cdecl

// Lifecycle
LV_CURL_API int LV_CURL_CALL lv_curl_global_init(char* error_buffer, int error_buffer_size);
LV_CURL_API int LV_CURL_CALL lv_curl_global_cleanup(char* error_buffer, int error_buffer_size);
LV_CURL_API const char* LV_CURL_CALL lv_curl_error_string(int error_code);

LV_CURL_API int LV_CURL_CALL lv_curl_open(
    const char* url,
    const char* headers,
    const char* username,
    const char* password,
    const char* bearer_token,
    int* reference,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_close(
    int reference,
    char* error_buffer,
    int error_buffer_size);

// Header helpers
LV_CURL_API int LV_CURL_CALL lv_curl_get_header_templates(
    char* headers_buffer,
    int headers_buffer_size,
    int* actual_headers_size,
    char* error_buffer,
    int error_buffer_size);

// Synchronous HTTP methods
LV_CURL_API int LV_CURL_CALL lv_curl_request(
    int method,
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_get(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_post(
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_put(
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_patch(
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_delete(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_head(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_options(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size);

// Async API
LV_CURL_API int LV_CURL_CALL lv_curl_async_get_start(
    int reference,
    const char* endpoint,
    const char* headers,
    int* request_id,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_async_post_start(
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    int* request_id,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_async_read_chunk(
    int request_id,
    char* chunk_buffer,
    int chunk_buffer_size,
    int* actual_chunk_size,
    int* is_last_chunk,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_async_get_state(
    int request_id,
    int* state,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size);

LV_CURL_API int LV_CURL_CALL lv_curl_async_cancel(
    int request_id,
    char* error_buffer,
    int error_buffer_size);

#ifdef __cplusplus
}
#endif
