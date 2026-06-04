#include "curl_lv_internal.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

std::atomic<bool> g_curl_global_initialized{false};
std::shared_mutex g_lifecycle_mutex;
std::mutex g_connection_mutex;
std::map<int, std::shared_ptr<CurlConnection>> g_connections;
std::atomic<int> g_next_connection_reference{1};
static std::mutex g_error_info_mutex;
static std::string g_error_info_queue;
static constexpr size_t LV_CURL_ERROR_INFO_MAX_BYTES = 8192;

int copy_c_string(const std::string& source, char* dest, int dest_size)
{
    if (!dest || dest_size <= 0)
    {
        return LV_CURL_ERROR_INVALID_ARGUMENT;
    }

    int required = static_cast<int>(source.size());
    if (required >= dest_size)
    {
        if (dest_size > 0)
        {
            std::memcpy(dest, source.c_str(), dest_size - 1);
            dest[dest_size - 1] = '\0';
        }
        return LV_CURL_ERROR_BUFFER_TOO_SMALL;
    }

    std::memcpy(dest, source.c_str(), required);
    dest[required] = '\0';
    return LV_CURL_SUCCESS;
}

int copy_binary_data(const std::string& source, char* dest, int dest_size)
{
    if (!dest || dest_size < 0)
    {
        return LV_CURL_ERROR_INVALID_ARGUMENT;
    }

    int required = static_cast<int>(source.size());
    if (required > dest_size)
    {
        return LV_CURL_ERROR_BUFFER_TOO_SMALL;
    }

    if (required > 0)
    {
        std::memcpy(dest, source.data(), required);
    }

    if (required < dest_size)
    {
        dest[required] = '\0';
    }

    return LV_CURL_SUCCESS;
}

static void trim_error_info_queue_to_capacity()
{
    if (g_error_info_queue.size() <= LV_CURL_ERROR_INFO_MAX_BYTES)
    {
        return;
    }

    size_t bytes_to_remove = g_error_info_queue.size() - LV_CURL_ERROR_INFO_MAX_BYTES;
    size_t newline_position = g_error_info_queue.find('\n', bytes_to_remove);
    if (newline_position != std::string::npos)
    {
        bytes_to_remove = newline_position + 1;
    }

    g_error_info_queue.erase(0, bytes_to_remove);
}

void push_error_info(const std::string& function_name, const std::string& message)
{
    if (message.empty())
    {
        return;
    }

    std::string entry = function_name.empty() ? message : function_name + ": " + message;
    entry += "\r\n";

    if (entry.size() > LV_CURL_ERROR_INFO_MAX_BYTES)
    {
        static const char truncation_marker[] = "[truncated] ";
        size_t marker_size = std::strlen(truncation_marker);
        size_t tail_size = LV_CURL_ERROR_INFO_MAX_BYTES > marker_size
            ? LV_CURL_ERROR_INFO_MAX_BYTES - marker_size
            : 0;
        entry = truncation_marker + entry.substr(entry.size() - tail_size);
    }

    std::lock_guard<std::mutex> guard(g_error_info_mutex);
    g_error_info_queue += entry;
    trim_error_info_queue_to_capacity();
}

int record_error_info(int error_code, const std::string& function_name, const std::string& message)
{
    push_error_info(function_name, message);
    return error_code;
}

std::vector<std::string> split_header_lines(const char* headers)
{
    std::vector<std::string> result;
    if (!headers)
    {
        return result;
    }

    std::string header_string(headers);
    size_t position = 0;

    while (position < header_string.size())
    {
        size_t end = header_string.find_first_of("\r\n", position);
        std::string line;

        if (end == std::string::npos)
        {
            line = header_string.substr(position);
            position = header_string.size();
        }
        else
        {
            line = header_string.substr(position, end - position);
            if (header_string[end] == '\r' && end + 1 < header_string.size() && header_string[end + 1] == '\n')
            {
                position = end + 2;
            }
            else
            {
                position = end + 1;
            }
        }

        if (!line.empty())
        {
            result.push_back(line);
        }
    }

    return result;
}

std::string build_request_url(const std::string& base_url, const char* endpoint)
{
    if (!endpoint || endpoint[0] == '\0')
    {
        return base_url;
    }

    std::string endpoint_string(endpoint);
    if (endpoint_string.rfind("http://", 0) == 0 || endpoint_string.rfind("https://", 0) == 0)
    {
        return endpoint_string;
    }

    if (base_url.empty())
    {
        return endpoint_string;
    }

    bool base_has_slash = base_url.back() == '/';
    bool endpoint_has_slash = endpoint_string.front() == '/';

    if (base_has_slash && endpoint_has_slash)
    {
        return base_url + endpoint_string.substr(1);
    }

    if (!base_has_slash && !endpoint_has_slash)
    {
        return base_url + "/" + endpoint_string;
    }

    return base_url + endpoint_string;
}

static size_t sync_write_callback(char* contents, size_t size, size_t nmemb, void* userdata)
{
    size_t total_size = size * nmemb;
    auto* response = static_cast<std::string*>(userdata);
    response->append(contents, total_size);
    return total_size;
}

static int curl_to_error_code(CURLcode code)
{
    if (code == CURLE_OK)
    {
        return LV_CURL_SUCCESS;
    }
    return LV_CURL_ERROR_CURL;
}

std::shared_ptr<CurlConnection> find_connection(int reference)
{
    std::lock_guard<std::mutex> guard(g_connection_mutex);
    auto it = g_connections.find(reference);
    if (it == g_connections.end())
    {
        return nullptr;
    }
    return it->second;
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

static int configure_connection(
    CURL* curl,
    const CurlConnection& connection,
    const std::string& request_url,
    const std::vector<std::string>& request_headers,
    struct curl_slist** header_list)
{
    curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str());

    int append_result = append_header_lines(header_list, connection.headers);
    if (append_result != LV_CURL_SUCCESS)
    {
        return append_result;
    }

    append_result = append_header_lines(header_list, request_headers);
    if (append_result != LV_CURL_SUCCESS)
    {
        return append_result;
    }

    if (!connection.bearer_token.empty())
    {
        std::string auth_header = "Authorization: Bearer " + connection.bearer_token;
        *header_list = curl_slist_append(*header_list, auth_header.c_str());
        if (!*header_list)
        {
            return LV_CURL_ERROR_INITIALIZATION;
        }
    }

    if (!connection.username.empty())
    {
        curl_easy_setopt(curl, CURLOPT_USERNAME, connection.username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, connection.password.c_str());
    }

    return LV_CURL_SUCCESS;
}

static int internal_curl_request(
    int method,
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code)
{
    if (reference <= 0 || !response_buffer || response_buffer_size <= 0 || !actual_response_size || !http_status_code)
    {
        return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_request", "Invalid argument.");
    }

    std::shared_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

    if (!g_curl_global_initialized.load())
    {
        return record_error_info(LV_CURL_ERROR_INITIALIZATION, "lv_curl_request", "curl_global_init has not been called.");
    }

    auto connection = find_connection(reference);
    if (!connection)
    {
        return record_error_info(LV_CURL_ERROR_NOT_FOUND, "lv_curl_request", "Reference not found.");
    }

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        return record_error_info(LV_CURL_ERROR_INITIALIZATION, "lv_curl_request", "Unable to initialize libcurl.");
    }

    std::string response_data;
    struct curl_slist* header_list = nullptr;
    std::string request_url = build_request_url(connection->url, endpoint);
    std::vector<std::string> request_headers = split_header_lines(headers);
    int configure_result = configure_connection(curl, *connection, request_url, request_headers, &header_list);
    if (configure_result != LV_CURL_SUCCESS)
    {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        return record_error_info(configure_result, "lv_curl_request", "Failed to allocate libcurl header list.");
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sync_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    switch (method)
    {
        case LV_CURL_METHOD_GET:
            break;
        case LV_CURL_METHOD_POST:
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            if (body)
            {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            }
            break;
        case LV_CURL_METHOD_PUT:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            if (body)
            {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            }
            break;
        case LV_CURL_METHOD_PATCH:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
            if (body)
            {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            }
            break;
        case LV_CURL_METHOD_DELETE:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            if (body)
            {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            }
            break;
        case LV_CURL_METHOD_HEAD:
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
            break;
        case LV_CURL_METHOD_OPTIONS:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "OPTIONS");
            break;
        default:
            curl_slist_free_all(header_list);
            curl_easy_cleanup(curl);
            return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_request", "Unsupported HTTP method.");
    }

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK)
    {
        std::string error_message = curl_easy_strerror(result);
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        int code = curl_to_error_code(result);
        return record_error_info(code, "lv_curl_request", error_message);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status_code);
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    *actual_response_size = static_cast<int>(response_data.size());

    int copy_result = copy_binary_data(response_data, response_buffer, response_buffer_size);
    if (copy_result != LV_CURL_SUCCESS)
    {
        return record_error_info(LV_CURL_ERROR_BUFFER_TOO_SMALL, "lv_curl_request", "Response buffer too small.");
    }

    return LV_CURL_SUCCESS;
}

int LV_CURL_CALL lv_curl_global_init(void)
{
    try
    {
        std::unique_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

        if (g_curl_global_initialized.load())
        {
            return LV_CURL_SUCCESS;
        }

        CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK)
        {
            return record_error_info(LV_CURL_ERROR_INITIALIZATION, "lv_curl_global_init", curl_easy_strerror(code));
        }

        g_curl_global_initialized.store(true);
        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_global_init", "Unexpected exception.");
    }
}

int LV_CURL_CALL lv_curl_global_cleanup(void)
{
    try
    {
        std::unique_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

        if (!g_curl_global_initialized.load())
        {
            return LV_CURL_SUCCESS;
        }

        extern std::mutex g_async_mutex;
        extern std::map<int, std::shared_ptr<AsyncRequest>> g_async_requests;

        {
            std::lock_guard<std::mutex> guard(g_async_mutex);
            for (const auto& pair : g_async_requests)
            {
                if (pair.second)
                {
                    std::lock_guard<std::mutex> request_guard(pair.second->mutex);
                    if (pair.second->state == LV_CURL_ASYNC_STATE_RUNNING)
                    {
                        return record_error_info(LV_CURL_ERROR_BUSY, "lv_curl_global_cleanup", "Cannot cleanup while async requests are running.");
                    }
                }
            }
        }

        curl_global_cleanup();
        g_curl_global_initialized.store(false);
        {
            std::lock_guard<std::mutex> guard(g_connection_mutex);
            g_connections.clear();
        }
        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_global_cleanup", "Unexpected exception.");
    }
}

static const char* error_string_literal(int error_code)
{
    switch (error_code)
    {
        case LV_CURL_SUCCESS: return "Success";
        case LV_CURL_ERROR_INVALID_ARGUMENT: return "Invalid argument";
        case LV_CURL_ERROR_INITIALIZATION: return "Initialization failure";
        case LV_CURL_ERROR_CURL: return "libcurl error";
        case LV_CURL_ERROR_BUFFER_TOO_SMALL: return "Buffer too small";
        case LV_CURL_ERROR_NOT_FOUND: return "Not found";
        case LV_CURL_ERROR_BUSY: return "Resource busy";
        case LV_CURL_ERROR_CANCELLED: return "Request cancelled";
        case LV_CURL_ERROR_INTERNAL: return "Internal error";
        case LV_CURL_ERROR_ASYNC_NOT_FOUND: return "Async request not found";
        case LV_CURL_ERROR_ASYNC_COMPLETE: return "Async request complete";
        case LV_CURL_ERROR_ASYNC_NO_CHUNK: return "No async chunk available";
        case LV_CURL_ERROR_ASYNC_FAILED: return "Async request failed";
        default: return "Unknown error code";
    }
}

int LV_CURL_CALL lv_curl_error_string(
    int error_code,
    char* error_string_buffer,
    int error_string_buffer_size,
    int* actual_error_string_size)
{
    if (!error_string_buffer || error_string_buffer_size <= 0 || !actual_error_string_size)
    {
        return LV_CURL_ERROR_INVALID_ARGUMENT;
    }

    std::string error_string(error_string_literal(error_code));
    *actual_error_string_size = static_cast<int>(error_string.size());

    int copy_result = copy_c_string(error_string, error_string_buffer, error_string_buffer_size);
    if (copy_result != LV_CURL_SUCCESS)
    {
        return copy_result;
    }

    return LV_CURL_SUCCESS;
}

int LV_CURL_CALL lv_curl_get_latest_errors_info(
    char* errors_buffer,
    int errors_buffer_size,
    int* actual_errors_size)
{
    if (!errors_buffer || errors_buffer_size <= 0 || !actual_errors_size)
    {
        return LV_CURL_ERROR_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> guard(g_error_info_mutex);
    *actual_errors_size = static_cast<int>(g_error_info_queue.size());

    int copy_result = copy_c_string(g_error_info_queue, errors_buffer, errors_buffer_size);
    if (copy_result != LV_CURL_SUCCESS)
    {
        return copy_result;
    }

    g_error_info_queue.clear();
    return LV_CURL_SUCCESS;
}

int LV_CURL_CALL lv_curl_open(
    const char* url,
    const char* headers,
    const char* username,
    const char* password,
    const char* bearer_token,
    int* reference)
{
    try
    {
        std::unique_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

        if (!url || !reference)
        {
            return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_open", "Invalid argument.");
        }

        if (!g_curl_global_initialized.load())
        {
            return record_error_info(LV_CURL_ERROR_INITIALIZATION, "lv_curl_open", "curl_global_init has not been called.");
        }

        int new_reference = g_next_connection_reference.fetch_add(1);
        if (new_reference <= 0)
        {
            return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_open", "Unable to reserve reference.");
        }

        auto connection = std::make_shared<CurlConnection>();
        connection->reference = new_reference;
        connection->url = url;
        connection->headers = split_header_lines(headers);
        connection->username = username ? username : "";
        connection->password = password ? password : "";
        connection->bearer_token = bearer_token ? bearer_token : "";

        {
            std::lock_guard<std::mutex> guard(g_connection_mutex);
            g_connections[new_reference] = connection;
        }

        *reference = new_reference;
        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_open", "Unexpected exception.");
    }
}

int LV_CURL_CALL lv_curl_close(int reference)
{
    try
    {
        std::unique_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

        if (reference <= 0)
        {
            return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_close", "Invalid argument.");
        }

        {
            std::lock_guard<std::mutex> guard(g_async_mutex);
            for (const auto& pair : g_async_requests)
            {
                if (pair.second && pair.second->reference == reference)
                {
                    std::lock_guard<std::mutex> request_guard(pair.second->mutex);
                    if (pair.second->state == LV_CURL_ASYNC_STATE_RUNNING)
                    {
                        return record_error_info(LV_CURL_ERROR_BUSY, "lv_curl_close", "Cannot close reference while async request is running.");
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> guard(g_connection_mutex);
            auto erased = g_connections.erase(reference);
            if (erased == 0)
            {
                return record_error_info(LV_CURL_ERROR_NOT_FOUND, "lv_curl_close", "Reference not found.");
            }
        }

        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_close", "Unexpected exception.");
    }
}

int LV_CURL_CALL lv_curl_get_header_templates(
    char* headers_buffer,
    int headers_buffer_size,
    int* actual_headers_size)
{
    try
    {
        if (!headers_buffer || headers_buffer_size <= 0 || !actual_headers_size)
        {
            return record_error_info(LV_CURL_ERROR_INVALID_ARGUMENT, "lv_curl_get_header_templates", "Invalid argument.");
        }

        static const char* header_templates =
            "Accept: application/json\r\n"
            "Accept: text/plain\r\n"
            "Accept: */*\r\n"
            "Content-Type: application/json\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Type: text/plain\r\n"
            "Authorization: Bearer <token>\r\n"
            "Authorization: Basic <base64-credentials>\r\n"
            "User-Agent: LabVIEW curl_lv\r\n"
            "Cache-Control: no-cache\r\n"
            "If-Match: <etag>\r\n"
            "If-None-Match: <etag>\r\n"
            "If-Modified-Since: <http-date>\r\n"
            "Range: bytes=<start>-<end>\r\n"
            "Cookie: <name>=<value>\r\n"
            "X-API-Key: <key>\r\n";

        std::string templates(header_templates);
        *actual_headers_size = static_cast<int>(templates.size());

        int copy_result = copy_c_string(templates, headers_buffer, headers_buffer_size);
        if (copy_result != LV_CURL_SUCCESS)
        {
            return record_error_info(copy_result, "lv_curl_get_header_templates", "Header template buffer too small.");
        }

        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_get_header_templates", "Unexpected exception.");
    }
}

int LV_CURL_CALL lv_curl_request(
    int method,
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code)
{
    try
    {
        return internal_curl_request(method, reference, endpoint, headers, body,
            response_buffer, response_buffer_size, actual_response_size,
            http_status_code);
    }
    catch (...)
    {
        return record_error_info(LV_CURL_ERROR_INTERNAL, "lv_curl_request", "Unexpected exception.");
    }
}

int LV_CURL_CALL lv_curl_get(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code)
{
    return lv_curl_request(LV_CURL_METHOD_GET, reference, endpoint, headers, nullptr,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code);
}

int LV_CURL_CALL lv_curl_post(
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code)
{
    return lv_curl_request(LV_CURL_METHOD_POST, reference, endpoint, headers, body,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code);
}

int LV_CURL_CALL lv_curl_put(
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code)
{
    return lv_curl_request(LV_CURL_METHOD_PUT, reference, endpoint, headers, body,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code);
}

int LV_CURL_CALL lv_curl_patch(
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code)
{
    return lv_curl_request(LV_CURL_METHOD_PATCH, reference, endpoint, headers, body,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code);
}

int LV_CURL_CALL lv_curl_delete(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code)
{
    return lv_curl_request(LV_CURL_METHOD_DELETE, reference, endpoint, headers, nullptr,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code);
}

int LV_CURL_CALL lv_curl_head(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code)
{
    return lv_curl_request(LV_CURL_METHOD_HEAD, reference, endpoint, headers, nullptr,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code);
}

int LV_CURL_CALL lv_curl_options(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code)
{
    return lv_curl_request(LV_CURL_METHOD_OPTIONS, reference, endpoint, headers, nullptr,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code);
}
