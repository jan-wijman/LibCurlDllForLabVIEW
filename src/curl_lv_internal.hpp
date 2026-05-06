#pragma once

#include "curl_lv.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <map>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

struct AsyncRequest
{
    int id = 0;
    int reference = 0;
    int state = 0;
    int http_status_code = 0;
    bool cancel_requested = false;
    bool completed = false;
    bool failed = false;
    std::string error_message;
    std::queue<std::string> chunk_queue;
    std::mutex mutex;
    std::condition_variable cv;
};

struct CurlConnection
{
    int reference = 0;
    std::string url;
    std::vector<std::string> headers;
    std::string username;
    std::string password;
    std::string bearer_token;
};

std::vector<std::string> split_header_lines(const char* headers);
int copy_c_string(const std::string& source, char* dest, int dest_size);
int set_error_message(const std::string& source, char* error_buffer, int error_buffer_size);
std::shared_ptr<CurlConnection> find_connection(int reference);

extern std::atomic<bool> g_curl_global_initialized;
extern std::shared_mutex g_lifecycle_mutex;
extern std::mutex g_connection_mutex;
extern std::map<int, std::shared_ptr<CurlConnection>> g_connections;
extern std::atomic<int> g_next_connection_reference;
extern std::mutex g_async_mutex;
extern std::map<int, std::shared_ptr<AsyncRequest>> g_async_requests;
extern std::atomic<int> g_next_request_id;

int reserve_async_request_id(int* request_id);
std::shared_ptr<AsyncRequest> find_async_request(int request_id);

void start_async_request(
    int request_id,
    std::shared_ptr<CurlConnection> connection,
    std::vector<std::string> request_headers,
    const std::string& body,
    bool use_post);
