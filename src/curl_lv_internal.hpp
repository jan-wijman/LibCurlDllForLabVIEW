#pragma once

#include "curl_lv.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

struct AsyncRequest
{
    int id = 0;
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

std::vector<std::string> split_header_lines(const char* headers);
int copy_c_string(const std::string& source, char* dest, int dest_size);
int set_error_message(const std::string& source, char* error_buffer, int error_buffer_size);

extern std::atomic<bool> g_curl_global_initialized;
extern std::mutex g_async_mutex;
extern std::map<int, std::shared_ptr<AsyncRequest>> g_async_requests;
extern std::atomic<int> g_next_request_id;

int reserve_async_request_id(int* request_id);
std::shared_ptr<AsyncRequest> find_async_request(int request_id);

void start_async_request(int request_id, const std::string& url, const std::vector<std::string>& headers, const std::string& body, bool use_post);
