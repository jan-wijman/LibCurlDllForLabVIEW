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

int set_error_message(const std::string& source, char* error_buffer, int error_buffer_size)
{
    if (!error_buffer || error_buffer_size <= 0)
    {
        return LV_CURL_ERROR_INVALID_ARGUMENT;
    }

    if (source.empty())
    {
        error_buffer[0] = '\0';
        return LV_CURL_SUCCESS;
    }

    return copy_c_string(source, error_buffer, error_buffer_size);
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
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size)
{
    if (reference <= 0 || !response_buffer || response_buffer_size <= 0 || !actual_response_size || !http_status_code || !error_buffer || error_buffer_size <= 0)
    {
        return LV_CURL_ERROR_INVALID_ARGUMENT;
    }

    std::shared_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

    if (!g_curl_global_initialized.load())
    {
        return set_error_message("curl_global_init has not been called.", error_buffer, error_buffer_size);
    }

    auto connection = find_connection(reference);
    if (!connection)
    {
        set_error_message("Reference not found.", error_buffer, error_buffer_size);
        return LV_CURL_ERROR_NOT_FOUND;
    }

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        return set_error_message("Unable to initialize libcurl.", error_buffer, error_buffer_size);
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
        return set_error_message("Failed to allocate libcurl header list.", error_buffer, error_buffer_size);
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
            return set_error_message("Unsupported HTTP method.", error_buffer, error_buffer_size);
    }

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK)
    {
        std::string error_message = curl_easy_strerror(result);
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        int code = curl_to_error_code(result);
        if (set_error_message(error_message, error_buffer, error_buffer_size) == LV_CURL_ERROR_BUFFER_TOO_SMALL)
        {
            return LV_CURL_ERROR_BUFFER_TOO_SMALL;
        }
        return code;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status_code);
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    *actual_response_size = static_cast<int>(response_data.size());

    int copy_result = copy_c_string(response_data, response_buffer, response_buffer_size);
    if (copy_result != LV_CURL_SUCCESS)
    {
        set_error_message("Response buffer too small.", error_buffer, error_buffer_size);
        return LV_CURL_ERROR_BUFFER_TOO_SMALL;
    }

    error_buffer[0] = '\0';
    return LV_CURL_SUCCESS;
}

int LV_CURL_CALL lv_curl_global_init(char* error_buffer, int error_buffer_size)
{
    try
    {
        std::unique_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

        if (!error_buffer || error_buffer_size <= 0)
        {
            return LV_CURL_ERROR_INVALID_ARGUMENT;
        }

        if (g_curl_global_initialized.load())
        {
            error_buffer[0] = '\0';
            return LV_CURL_SUCCESS;
        }

        CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK)
        {
            return set_error_message(curl_easy_strerror(code), error_buffer, error_buffer_size);
        }

        g_curl_global_initialized.store(true);
        error_buffer[0] = '\0';
        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return set_error_message("Unexpected exception in lv_curl_global_init.", error_buffer, error_buffer_size);
    }
}

int LV_CURL_CALL lv_curl_global_cleanup(char* error_buffer, int error_buffer_size)
{
    try
    {
        std::unique_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

        if (!error_buffer || error_buffer_size <= 0)
        {
            return LV_CURL_ERROR_INVALID_ARGUMENT;
        }

        if (!g_curl_global_initialized.load())
        {
            error_buffer[0] = '\0';
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
                        return set_error_message("Cannot cleanup while async requests are running.", error_buffer, error_buffer_size);
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
        error_buffer[0] = '\0';
        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return set_error_message("Unexpected exception in lv_curl_global_cleanup.", error_buffer, error_buffer_size);
    }
}

const char* LV_CURL_CALL lv_curl_error_string(int error_code)
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

int LV_CURL_CALL lv_curl_open(
    const char* url,
    const char* headers,
    const char* username,
    const char* password,
    const char* bearer_token,
    int* reference,
    char* error_buffer,
    int error_buffer_size)
{
    try
    {
        std::unique_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

        if (!url || !reference || !error_buffer || error_buffer_size <= 0)
        {
            return LV_CURL_ERROR_INVALID_ARGUMENT;
        }

        if (!g_curl_global_initialized.load())
        {
            set_error_message("curl_global_init has not been called.", error_buffer, error_buffer_size);
            return LV_CURL_ERROR_INITIALIZATION;
        }

        int new_reference = g_next_connection_reference.fetch_add(1);
        if (new_reference <= 0)
        {
            set_error_message("Unable to reserve reference.", error_buffer, error_buffer_size);
            return LV_CURL_ERROR_INTERNAL;
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
        error_buffer[0] = '\0';
        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return set_error_message("Unexpected exception in lv_curl_open.", error_buffer, error_buffer_size);
    }
}

int LV_CURL_CALL lv_curl_close(int reference, char* error_buffer, int error_buffer_size)
{
    try
    {
        std::unique_lock<std::shared_mutex> lifecycle_guard(g_lifecycle_mutex);

        if (reference <= 0 || !error_buffer || error_buffer_size <= 0)
        {
            return LV_CURL_ERROR_INVALID_ARGUMENT;
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
                        set_error_message("Cannot close reference while async request is running.", error_buffer, error_buffer_size);
                        return LV_CURL_ERROR_BUSY;
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> guard(g_connection_mutex);
            auto erased = g_connections.erase(reference);
            if (erased == 0)
            {
                set_error_message("Reference not found.", error_buffer, error_buffer_size);
                return LV_CURL_ERROR_NOT_FOUND;
            }
        }

        error_buffer[0] = '\0';
        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return set_error_message("Unexpected exception in lv_curl_close.", error_buffer, error_buffer_size);
    }
}

int LV_CURL_CALL lv_curl_get_header_templates(
    char* headers_buffer,
    int headers_buffer_size,
    int* actual_headers_size,
    char* error_buffer,
    int error_buffer_size)
{
    try
    {
        if (!headers_buffer || headers_buffer_size <= 0 || !actual_headers_size || !error_buffer || error_buffer_size <= 0)
        {
            return LV_CURL_ERROR_INVALID_ARGUMENT;
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
            set_error_message("Header template buffer too small.", error_buffer, error_buffer_size);
            return copy_result;
        }

        error_buffer[0] = '\0';
        return LV_CURL_SUCCESS;
    }
    catch (...)
    {
        return set_error_message("Unexpected exception in lv_curl_get_header_templates.", error_buffer, error_buffer_size);
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
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size)
{
    try
    {
        return internal_curl_request(method, reference, endpoint, headers, body,
            response_buffer, response_buffer_size, actual_response_size,
            http_status_code, error_buffer, error_buffer_size);
    }
    catch (...)
    {
        return set_error_message("Unexpected exception in lv_curl_request.", error_buffer, error_buffer_size);
    }
}

int LV_CURL_CALL lv_curl_get(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size)
{
    return lv_curl_request(LV_CURL_METHOD_GET, reference, endpoint, headers, nullptr,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code, error_buffer, error_buffer_size);
}

int LV_CURL_CALL lv_curl_post(
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size)
{
    return lv_curl_request(LV_CURL_METHOD_POST, reference, endpoint, headers, body,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code, error_buffer, error_buffer_size);
}

int LV_CURL_CALL lv_curl_put(
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size)
{
    return lv_curl_request(LV_CURL_METHOD_PUT, reference, endpoint, headers, body,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code, error_buffer, error_buffer_size);
}

int LV_CURL_CALL lv_curl_patch(
    int reference,
    const char* endpoint,
    const char* headers,
    const char* body,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size)
{
    return lv_curl_request(LV_CURL_METHOD_PATCH, reference, endpoint, headers, body,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code, error_buffer, error_buffer_size);
}

int LV_CURL_CALL lv_curl_delete(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size)
{
    return lv_curl_request(LV_CURL_METHOD_DELETE, reference, endpoint, headers, nullptr,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code, error_buffer, error_buffer_size);
}

int LV_CURL_CALL lv_curl_head(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size)
{
    return lv_curl_request(LV_CURL_METHOD_HEAD, reference, endpoint, headers, nullptr,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code, error_buffer, error_buffer_size);
}

int LV_CURL_CALL lv_curl_options(
    int reference,
    const char* endpoint,
    const char* headers,
    char* response_buffer,
    int response_buffer_size,
    int* actual_response_size,
    int* http_status_code,
    char* error_buffer,
    int error_buffer_size)
{
    return lv_curl_request(LV_CURL_METHOD_OPTIONS, reference, endpoint, headers, nullptr,
        response_buffer, response_buffer_size, actual_response_size,
        http_status_code, error_buffer, error_buffer_size);
}
