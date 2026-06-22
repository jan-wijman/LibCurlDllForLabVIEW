#include "curl_lv_internal.hpp"
#include <curl/curl.h>
#include <chrono>
#include <cstring>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

std::mutex g_async_mutex;
std::map<int, std::shared_ptr<AsyncRequest>> g_async_requests;
std::atomic<int> g_next_request_id{1};

int reserve_async_request_id(int* request_id)
{
    if (!request_id)
    {
        return LV_CURL_ERROR_INVALID_ARGUMENT;
    }

    int id = g_next_request_id.fetch_add(1);
    if (id <= 0)
    {
        return LV_CURL_ERROR_INTERNAL;
    }

    *request_id = id;
    return LV_CURL_SUCCESS;
}

std::shared_ptr<AsyncRequest> find_async_request(int request_id)
{
    std::lock_guard<std::mutex> guard(g_async_mutex);
    auto it = g_async_requests.find(request_id);
    if (it == g_async_requests.end())
    {
        return nullptr;
    }
    return it->second;
}

static int async_progress_callback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto* request = static_cast<AsyncRequest*>(clientp);
    if (!request)
    {
        return 0;
    }

    std::lock_guard<std::mutex> guard(request->mutex);
    return request->cancel_requested ? 1 : 0;
}

static size_t async_write_callback(char* contents, size_t size, size_t nmemb, void* userdata)
{
    size_t total_size = size * nmemb;
    auto* request = static_cast<AsyncRequest*>(userdata);
    if (!request)
    {
        return 0;
    }

    {
        std::lock_guard<std::mutex> guard(request->mutex);
        if (request->cancel_requested)
        {
            return 0;
        }

        request->chunk_queue.emplace(contents, total_size);
    }
    request->cv.notify_all();
    return total_size;
}

static int append_header_lines(struct curl_slist** header_list, const std::vector<std::string>& header_lines)
{
    for (const auto& header_line : header_lines)
    {
        *header_list = curl_slist_append(*header_list, header_line.c_str());
        if (!*header_list)
        {
            return LV_CURL_ERROR_INITIALIZATION;
        }
    }

    return LV_CURL_SUCCESS;
}

void start_async_request(
    int request_id,
    std::shared_ptr<CurlConnection> connection,
    std::string request_url,
    std::vector<std::string> request_headers,
    const std::string& body,
    bool use_post)
{
    auto request = find_async_request(request_id);
    if (!request || !connection)
    {
        return;
    }

    std::thread worker([request, connection, request_url, request_headers, body, use_post]() {
        CURL* curl = curl_easy_init();
        if (!curl)
        {
            std::lock_guard<std::mutex> guard(request->mutex);
            request->failed = true;
            request->state = LV_CURL_ASYNC_STATE_FAILED;
            request->error_message = "Unable to initialize libcurl.";
            push_error_info("lv_curl_async_worker", request->error_message);
            request->cv.notify_all();
            return;
        }

        struct curl_slist* header_list = nullptr;
        int append_result = append_header_lines(&header_list, connection->headers);
        if (append_result != LV_CURL_SUCCESS)
        {
            curl_easy_cleanup(curl);
            std::lock_guard<std::mutex> guard(request->mutex);
            request->failed = true;
            request->state = LV_CURL_ASYNC_STATE_FAILED;
            request->error_message = "Failed to allocate libcurl header list.";
            push_error_info("lv_curl_async_worker", request->error_message);
            request->cv.notify_all();
            return;
        }

        append_result = append_header_lines(&header_list, request_headers);
        if (append_result != LV_CURL_SUCCESS)
        {
            curl_slist_free_all(header_list);
            curl_easy_cleanup(curl);
            std::lock_guard<std::mutex> guard(request->mutex);
            request->failed = true;
            request->state = LV_CURL_ASYNC_STATE_FAILED;
            request->error_message = "Failed to allocate libcurl header list.";
            push_error_info("lv_curl_async_worker", request->error_message);
            request->cv.notify_all();
            return;
        }

        if (!connection->bearer_token.empty())
        {
            std::string auth_header = "Authorization: Bearer " + connection->bearer_token;
            header_list = curl_slist_append(header_list, auth_header.c_str());
            if (!header_list)
            {
                curl_easy_cleanup(curl);
                std::lock_guard<std::mutex> guard(request->mutex);
                request->failed = true;
                request->state = LV_CURL_ASYNC_STATE_FAILED;
                request->error_message = "Failed to allocate libcurl header list.";
                push_error_info("lv_curl_async_worker", request->error_message);
                request->cv.notify_all();
                return;
            }
        }

        curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, async_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, request.get());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, async_progress_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, request.get());
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        if (!connection->username.empty())
        {
            curl_easy_setopt(curl, CURLOPT_USERNAME, connection->username.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, connection->password.c_str());
        }

        if (use_post)
        {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            if (!body.empty())
            {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            }
        }

        {
            std::lock_guard<std::mutex> guard(request->mutex);
            request->state = LV_CURL_ASYNC_STATE_RUNNING;
        }

        CURLcode result = curl_easy_perform(curl);
        long status_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);

        {
            std::lock_guard<std::mutex> guard(request->mutex);
            if (request->cancel_requested && (result == CURLE_ABORTED_BY_CALLBACK || result == CURLE_WRITE_ERROR))
            {
                request->state = LV_CURL_ASYNC_STATE_CANCELLED;
            }
            else if (result != CURLE_OK)
            {
                request->failed = true;
                request->state = LV_CURL_ASYNC_STATE_FAILED;
                request->error_message = curl_easy_strerror(result);
                push_error_info("lv_curl_async_worker", request->error_message);
            }
            else
            {
                request->state = LV_CURL_ASYNC_STATE_COMPLETED;
            }
            request->http_status_code = static_cast<int>(status_code);
            request->completed = true;
        }

        request->cv.notify_all();
    });

    worker.detach();
}

static int validate_async_buffer_args(char* buffer, int buffer_size, int* actual_size)
{
    if (!buffer || buffer_size <= 0 || !actual_size)
    {
        return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_async_read_chunk", "Invalid argument.");
    }
    return LV_CURL_SUCCESS;
}

int LV_CURL_CALL lv_curl_async_get_start(
    int reference,
    const char* endpoint,
    const char* headers,
    int* request_id)
{
    try
    {
        if (reference <= 0 || !request_id)
        {
            return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_async_get_start", "Invalid argument.");
        }

        std::shared_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

        if (!g_curl_global_initialized.load())
        {
            return record_error_info(LV_CURL_ERROR_INITIALIZATION, "lv_curl_async_get_start", "curl_global_init has not been called.");
        }

        auto connection = find_connection(reference);
        if (!connection)
        {
            return record_error_info(LV_CURL_ERROR_NOT_FOUND, "lv_curl_async_get_start", "Reference not found.");
        }

        int id;
        int code = reserve_async_request_id(&id);
        if (code != LV_CURL_SUCCESS)
        {
            return record_error_info(code, "lv_curl_async_get_start", "Unable to reserve async request id.");
        }

        auto request = std::make_shared<AsyncRequest>();
        request->id = id;
        request->reference = reference;
        request->state = LV_CURL_ASYNC_STATE_RUNNING;

        {
            std::lock_guard<std::mutex> guard(g_async_mutex);
            g_async_requests[id] = request;
        }

        std::vector<std::string> request_headers = split_header_lines(headers);
        std::string request_url = build_request_url(connection->url, endpoint);
        start_async_request(id, connection, request_url, request_headers, std::string(), false);

        *request_id = id;
        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_async_get_start", "Unexpected exception.");
    }
}

int LV_CURL_CALL lv_curl_async_post_start(
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    int* request_id)
{
    try
    {
        if (reference <= 0 || !request_id)
        {
            return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_async_post_start", "Invalid argument.");
        }

        std::shared_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

        if (!g_curl_global_initialized.load())
        {
            return record_error_info(LV_CURL_ERROR_INITIALIZATION, "lv_curl_async_post_start", "curl_global_init has not been called.");
        }

        auto connection = find_connection(reference);
        if (!connection)
        {
            return record_error_info(LV_CURL_ERROR_NOT_FOUND, "lv_curl_async_post_start", "Reference not found.");
        }

        int id;
        int code = reserve_async_request_id(&id);
        if (code != LV_CURL_SUCCESS)
        {
            return record_error_info(code, "lv_curl_async_post_start", "Unable to reserve async request id.");
        }

        auto request = std::make_shared<AsyncRequest>();
        request->id = id;
        request->reference = reference;
        request->state = LV_CURL_ASYNC_STATE_RUNNING;

        {
            std::lock_guard<std::mutex> guard(g_async_mutex);
            g_async_requests[id] = request;
        }

        std::vector<std::string> request_headers = split_header_lines(headers);
        std::string request_url = build_request_url(connection->url, endpoint);
        start_async_request(id, connection, request_url, request_headers, body ? std::string(body) : std::string(), true);

        *request_id = id;
        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_async_post_start", "Unexpected exception.");
    }
}

int LV_CURL_CALL lv_curl_async_read_chunk(
    int request_id,
    char* chunk_buffer,
    int chunk_buffer_size,
    int* actual_chunk_size,
    int* is_last_chunk)
{
    try
    {
        int validate_code = validate_async_buffer_args(chunk_buffer, chunk_buffer_size, actual_chunk_size);
        if (validate_code != LV_CURL_SUCCESS)
        {
            return validate_code;
        }

        if (!is_last_chunk)
        {
            return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_async_read_chunk", "Invalid argument.");
        }

        *actual_chunk_size = 0;
        *is_last_chunk = 0;

        auto request = find_async_request(request_id);
        if (!request)
        {
            return record_error_info(LV_CURL_ERROR_ASYNC_NOT_FOUND, "lv_curl_async_read_chunk", "Async request not found.");
        }

        std::unique_lock<std::mutex> guard(request->mutex);
        if (!request->chunk_queue.empty())
        {
            const std::string& chunk = request->chunk_queue.front();
            *actual_chunk_size = static_cast<int>(chunk.size());

            if (static_cast<int>(chunk.size()) > chunk_buffer_size)
            {
                return record_error_info(LV_CURL_ERROR_BUFFER_TOO_SMALL, "lv_curl_async_read_chunk", "Chunk buffer too small.");
            }

            std::memcpy(chunk_buffer, chunk.data(), chunk.size());
            if (static_cast<int>(chunk.size()) < chunk_buffer_size)
            {
                chunk_buffer[chunk.size()] = '\0';
            }
            request->chunk_queue.pop();

            *is_last_chunk = (request->completed && request->chunk_queue.empty()) ? 1 : 0;
            return LV_CURL_SUCCESS;
        }

        if (request->failed)
        {
            *actual_chunk_size = 0;
            *is_last_chunk = 0;
            return record_error_info(LV_CURL_ERROR_ASYNC_FAILED, "lv_curl_async_read_chunk", request->error_message);
        }

        if (request->cancel_requested && request->chunk_queue.empty())
        {
            return record_error_info(LV_CURL_ERROR_CANCELLED, "lv_curl_async_read_chunk", "Async request cancelled.");
        }

        if (request->completed)
        {
            return LV_CURL_ERROR_ASYNC_COMPLETE;
        }

        return LV_CURL_ERROR_ASYNC_NO_CHUNK;
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_async_read_chunk", "Unexpected exception.");
    }
}

int LV_CURL_CALL lv_curl_async_get_state(
    int request_id,
    int* state,
    int* http_status_code)
{
    try
    {
        if (!state || !http_status_code)
        {
            return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_async_get_state", "Invalid argument.");
        }

        *state = 0;
        *http_status_code = 0;

        auto request = find_async_request(request_id);
        if (!request)
        {
            return record_error_info(LV_CURL_ERROR_ASYNC_NOT_FOUND, "lv_curl_async_get_state", "Async request not found.");
        }

        std::lock_guard<std::mutex> guard(request->mutex);
        *state = request->state;
        *http_status_code = request->http_status_code;
        if (request->failed)
        {
            return record_error_info(LV_CURL_ERROR_ASYNC_FAILED, "lv_curl_async_get_state", request->error_message);
        }

        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_async_get_state", "Unexpected exception.");
    }
}

int LV_CURL_CALL lv_curl_async_cancel(int request_id)
{
    try
    {
        if (request_id <= 0)
        {
            return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_async_cancel", "Invalid argument.");
        }

        auto request = find_async_request(request_id);
        if (!request)
        {
            return record_error_info(LV_CURL_ERROR_ASYNC_NOT_FOUND, "lv_curl_async_cancel", "Async request not found.");
        }

        {
            std::lock_guard<std::mutex> guard(request->mutex);
            request->cancel_requested = true;
        }

        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_async_cancel", "Unexpected exception.");
    }
}

int LV_CURL_CALL lv_curl_async_release(int request_id)
{
    try
    {
        if (request_id <= 0)
        {
            return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_async_release", "Invalid argument.");
        }

        std::shared_ptr<AsyncRequest> request;
        {
            std::lock_guard<std::mutex> guard(g_async_mutex);
            auto it = g_async_requests.find(request_id);
            if (it == g_async_requests.end())
            {
                return record_error_info(LV_CURL_ERROR_ASYNC_NOT_FOUND, "lv_curl_async_release", "Async request not found.");
            }
            request = it->second;
        }

        {
            std::lock_guard<std::mutex> request_guard(request->mutex);
            if (request->state == LV_CURL_ASYNC_STATE_RUNNING)
            {
                return record_error_info(LV_CURL_ERROR_BUSY, "lv_curl_async_release", "Async request is still running.");
            }
        }

        std::lock_guard<std::mutex> guard(g_async_mutex);
        auto it = g_async_requests.find(request_id);
        if (it == g_async_requests.end())
        {
            return record_error_info(LV_CURL_ERROR_ASYNC_NOT_FOUND, "lv_curl_async_release", "Async request not found.");
        }

        {
            std::lock_guard<std::mutex> request_guard(it->second->mutex);
            if (it->second->state == LV_CURL_ASYNC_STATE_RUNNING)
            {
                return record_error_info(LV_CURL_ERROR_BUSY, "lv_curl_async_release", "Async request is still running.");
            }
        }

        g_async_requests.erase(it);
        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_async_release", "Unexpected exception.");
    }
}
