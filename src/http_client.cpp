#include "http_client.hpp"
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace polymarket
{

    // Global initialization
    static bool g_curl_initialized = false;

    void http_global_init()
    {
        if (!g_curl_initialized)
        {
            curl_global_init(CURL_GLOBAL_ALL);
            g_curl_initialized = true;
        }
    }

    void http_global_cleanup()
    {
        if (g_curl_initialized)
        {
            curl_global_cleanup();
            g_curl_initialized = false;
        }
    }

    HttpClient::HttpClient()
        : curl_(nullptr), multi_(nullptr), headers_(nullptr), timeout_ms_(5000),
          dns_cache_timeout_(60), keepalive_interval_(20),
          heartbeat_running_(false),
          async_running_(false),
          total_requests_(0), reused_connections_(0),
          total_latency_ms_(0.0), last_latency_ms_(0.0),
          connection_warm_(false)
    {
        init();
    }

    HttpClient::~HttpClient()
    {
        stop_heartbeat();
        stop_async_worker();
        cleanup();
    }

    HttpClient::HttpClient(HttpClient &&other) noexcept
        : curl_(other.curl_), multi_(other.multi_), headers_(other.headers_),
          base_url_(std::move(other.base_url_)),
          proxy_url_(std::move(other.proxy_url_)), user_agent_(std::move(other.user_agent_)),
          timeout_ms_(other.timeout_ms_),
          dns_cache_timeout_(other.dns_cache_timeout_), keepalive_interval_(other.keepalive_interval_),
          heartbeat_running_(false),
          total_requests_(other.total_requests_), reused_connections_(other.reused_connections_),
          total_latency_ms_(other.total_latency_ms_), last_latency_ms_(other.last_latency_ms_),
          connection_warm_(other.connection_warm_),
          pending_async_requests_(std::move(other.pending_async_requests_))
    {
        other.stop_heartbeat();
        other.stop_async_worker();
        other.curl_ = nullptr;
        other.multi_ = nullptr;
        other.headers_ = nullptr;
    }

    HttpClient &HttpClient::operator=(HttpClient &&other) noexcept
    {
        if (this != &other)
        {
            stop_heartbeat();
            other.stop_heartbeat();
            stop_async_worker();
            other.stop_async_worker();
            cleanup();
            curl_ = other.curl_;
            multi_ = other.multi_;
            headers_ = other.headers_;
            base_url_ = std::move(other.base_url_);
            proxy_url_ = std::move(other.proxy_url_);
            user_agent_ = std::move(other.user_agent_);
            timeout_ms_ = other.timeout_ms_;
            dns_cache_timeout_ = other.dns_cache_timeout_;
            keepalive_interval_ = other.keepalive_interval_;
            total_requests_ = other.total_requests_;
            reused_connections_ = other.reused_connections_;
            total_latency_ms_ = other.total_latency_ms_;
            last_latency_ms_ = other.last_latency_ms_;
            connection_warm_ = other.connection_warm_;
            pending_async_requests_ = std::move(other.pending_async_requests_);
            other.curl_ = nullptr;
            other.multi_ = nullptr;
            other.headers_ = nullptr;
        }
        return *this;
    }

    void HttpClient::init()
    {
        curl_ = curl_easy_init();
        if (!curl_)
        {
            throw std::runtime_error("Failed to initialize CURL");
        }
        multi_ = curl_multi_init();
        if (!multi_)
        {
            curl_easy_cleanup(curl_);
            curl_ = nullptr;
            throw std::runtime_error("Failed to initialize CURL multi");
        }

        // Set default options for performance
        apply_common_options(curl_);

        // Default headers
        add_header("Connection: keep-alive");
        add_header("Accept: application/json");
        add_header("Content-Type: application/json");
    }

    void HttpClient::cleanup()
    {
        for (auto &entry : pending_async_requests_)
        {
            if (multi_ && entry.first)
            {
                curl_multi_remove_handle(multi_, entry.first);
            }
            if (entry.second && entry.second->headers)
            {
                curl_slist_free_all(entry.second->headers);
            }
            if (entry.first)
            {
                curl_easy_cleanup(entry.first);
            }
        }
        pending_async_requests_.clear();
        if (multi_)
        {
            curl_multi_cleanup(multi_);
            multi_ = nullptr;
        }
        if (headers_)
        {
            curl_slist_free_all(headers_);
            headers_ = nullptr;
        }
        if (curl_)
        {
            curl_easy_cleanup(curl_);
            curl_ = nullptr;
        }
    }

    void HttpClient::set_timeout_ms(long timeout_ms)
    {
        timeout_ms_ = timeout_ms;
    }

    void HttpClient::set_base_url(const std::string &base_url)
    {
        base_url_ = base_url;
        // Remove trailing slash
        if (!base_url_.empty() && base_url_.back() == '/')
        {
            base_url_.pop_back();
        }
    }

    void HttpClient::add_header(const std::string &header)
    {
        headers_ = curl_slist_append(headers_, header.c_str());
    }

    void HttpClient::set_proxy(const std::string &proxy_url)
    {
        proxy_url_ = proxy_url;
        if (!proxy_url_.empty() && curl_)
        {
            curl_easy_setopt(curl_, CURLOPT_PROXY, proxy_url_.c_str());

            // Detect proxy type from URL scheme
            if (proxy_url_.find("socks5://") == 0 || proxy_url_.find("socks5h://") == 0)
            {
                // SOCKS5 proxy - use socks5h for DNS resolution through proxy
                curl_easy_setopt(curl_, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
            }
            else if (proxy_url_.find("socks4://") == 0)
            {
                curl_easy_setopt(curl_, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
            }
            else
            {
                // HTTP/HTTPS proxy
                curl_easy_setopt(curl_, CURLOPT_HTTPPROXYTUNNEL, 1L); // Use CONNECT for HTTPS
                curl_easy_setopt(curl_, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
            }

            // Skip SSL verification when using proxy (residential proxies may intercept)
            curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl_, CURLOPT_PROXY_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl_, CURLOPT_PROXY_SSL_VERIFYHOST, 0L);
        }
    }

    void HttpClient::set_user_agent(const std::string &user_agent)
    {
        user_agent_ = user_agent;
        if (curl_)
        {
            curl_easy_setopt(curl_, CURLOPT_USERAGENT, user_agent.c_str());
        }
    }

    void HttpClient::set_dns_cache_timeout(long seconds)
    {
        dns_cache_timeout_ = seconds;
        if (curl_)
        {
            curl_easy_setopt(curl_, CURLOPT_DNS_CACHE_TIMEOUT, seconds);
        }
    }

    void HttpClient::apply_common_options(CURL *handle)
    {
        curl_easy_setopt(handle, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, keepalive_interval_);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, keepalive_interval_);
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);

        curl_easy_setopt(handle, CURLOPT_FORBID_REUSE, 0L);
        curl_easy_setopt(handle, CURLOPT_FRESH_CONNECT, 0L);
        curl_easy_setopt(handle, CURLOPT_DNS_CACHE_TIMEOUT, dns_cache_timeout_);

        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);

        if (!user_agent_.empty())
        {
            curl_easy_setopt(handle, CURLOPT_USERAGENT, user_agent_.c_str());
        }

        if (!proxy_url_.empty())
        {
            curl_easy_setopt(handle, CURLOPT_PROXY, proxy_url_.c_str());
            if (proxy_url_.find("socks5://") == 0 || proxy_url_.find("socks5h://") == 0)
            {
                curl_easy_setopt(handle, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
            }
            else if (proxy_url_.find("socks4://") == 0)
            {
                curl_easy_setopt(handle, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
            }
            else
            {
                curl_easy_setopt(handle, CURLOPT_HTTPPROXYTUNNEL, 1L);
                curl_easy_setopt(handle, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
            }

            curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(handle, CURLOPT_PROXY_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(handle, CURLOPT_PROXY_SSL_VERIFYHOST, 0L);
        }
    }

    curl_slist *HttpClient::build_headers(const std::map<std::string, std::string> *custom_headers) const
    {
        curl_slist *temp_headers = nullptr;
        for (auto h = headers_; h; h = h->next)
        {
            temp_headers = curl_slist_append(temp_headers, h->data);
        }
        if (custom_headers)
        {
            for (const auto &[key, value] : *custom_headers)
            {
                std::string header = key + ": " + value;
                temp_headers = curl_slist_append(temp_headers, header.c_str());
            }
        }
        return temp_headers;
    }

    void HttpClient::enqueue_async_request(std::unique_ptr<AsyncRequest> request)
    {
        if (!multi_)
        {
            throw std::runtime_error("CURL multi not initialized");
        }
        CURL *easy = request->easy;
        curl_easy_setopt(easy, CURLOPT_PRIVATE, request.get());
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            curl_multi_add_handle(multi_, easy);
            pending_async_requests_[easy] = std::move(request);
        }
        ensure_async_worker();
        async_cv_.notify_one();
    }

    void HttpClient::set_keepalive_interval(long seconds)
    {
        keepalive_interval_ = seconds;
        if (curl_)
        {
            curl_easy_setopt(curl_, CURLOPT_TCP_KEEPIDLE, seconds);
            curl_easy_setopt(curl_, CURLOPT_TCP_KEEPINTVL, seconds);
        }
    }

    size_t HttpClient::write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
    {
        auto *response = static_cast<std::string *>(userdata);
        size_t total_size = size * nmemb;
        response->append(ptr, total_size);
        return total_size;
    }

    HttpResponse HttpClient::perform(const std::string &url)
    {
        HttpResponse response;
        response.status_code = 0;

        auto start = std::chrono::high_resolution_clock::now();

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, timeout_ms_);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response.body);

        CURLcode res = curl_easy_perform(curl_);

        auto end = std::chrono::high_resolution_clock::now();
        response.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();

        if (res != CURLE_OK)
        {
            response.error = curl_easy_strerror(res);
            return response;
        }

        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &response.status_code);

        // Track connection reuse stats
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            total_requests_++;
            total_latency_ms_ += response.elapsed_ms;
            last_latency_ms_ = response.elapsed_ms;

            // Check if connection was reused
            long reused = 0;
            curl_easy_getinfo(curl_, CURLINFO_NUM_CONNECTS, &reused);
            if (reused == 0)
            {
                reused_connections_++;
            }
        }

        return response;
    }

    HttpResponse HttpClient::get(const std::string &path)
    {
        std::string url = base_url_.empty() ? path : base_url_ + path;

        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl_, CURLOPT_POST, 0L);

        return perform(url);
    }

    HttpResponse HttpClient::get(const std::string &path, const std::map<std::string, std::string> &custom_headers)
    {
        // Save original headers
        struct curl_slist *original_headers = headers_;

        // Add custom headers
        struct curl_slist *temp_headers = nullptr;
        for (auto h = headers_; h; h = h->next)
        {
            temp_headers = curl_slist_append(temp_headers, h->data);
        }
        for (const auto &[key, value] : custom_headers)
        {
            std::string header = key + ": " + value;
            temp_headers = curl_slist_append(temp_headers, header.c_str());
        }
        headers_ = temp_headers;

        auto response = get(path);

        // Restore original headers
        curl_slist_free_all(headers_);
        headers_ = original_headers;

        return response;
    }

    HttpResponse HttpClient::post(const std::string &path, const std::string &body)
    {
        std::string url = base_url_.empty() ? path : base_url_ + path;

        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

        return perform(url);
    }

    HttpResponse HttpClient::post(const std::string &path, const std::string &body, const std::map<std::string, std::string> &custom_headers)
    {
        // Save original headers
        struct curl_slist *original_headers = headers_;

        // Add custom headers
        struct curl_slist *temp_headers = nullptr;
        for (auto h = headers_; h; h = h->next)
        {
            temp_headers = curl_slist_append(temp_headers, h->data);
        }
        for (const auto &[key, value] : custom_headers)
        {
            std::string header = key + ": " + value;
            temp_headers = curl_slist_append(temp_headers, header.c_str());
        }
        headers_ = temp_headers;

        auto response = post(path, body);

        // Restore original headers
        curl_slist_free_all(headers_);
        headers_ = original_headers;

        return response;
    }

    HttpResponse HttpClient::del(const std::string &path, const std::string &body)
    {
        std::string url = base_url_.empty() ? path : base_url_ + path;

        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (!body.empty())
        {
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }

        auto response = perform(url);

        // Reset to default
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, nullptr);

        return response;
    }

    HttpResponse HttpClient::del(const std::string &path, const std::string &body, const std::map<std::string, std::string> &custom_headers)
    {
        // Save original headers
        struct curl_slist *original_headers = headers_;

        // Add custom headers
        struct curl_slist *temp_headers = nullptr;
        for (auto h = headers_; h; h = h->next)
        {
            temp_headers = curl_slist_append(temp_headers, h->data);
        }
        for (const auto &[key, value] : custom_headers)
        {
            std::string header = key + ": " + value;
            temp_headers = curl_slist_append(temp_headers, header.c_str());
        }
        headers_ = temp_headers;

        auto response = del(path, body);

        // Restore original headers
        curl_slist_free_all(headers_);
        headers_ = original_headers;

        return response;
    }

    void HttpClient::get_async(const std::string &path, AsyncCallback callback)
    {
        std::string url = base_url_.empty() ? path : base_url_ + path;

        auto request = std::make_unique<AsyncRequest>();
        request->easy = curl_easy_init();
        if (!request->easy)
        {
            if (callback)
            {
                HttpResponse response;
                response.status_code = 0;
                response.error = "Failed to initialize CURL handle";
                callback(response);
            }
            return;
        }
        request->url = url;
        request->callback = std::move(callback);
        request->start = std::chrono::high_resolution_clock::now();
        request->headers = build_headers(nullptr);

        apply_common_options(request->easy);
        curl_easy_setopt(request->easy, CURLOPT_URL, request->url.c_str());
        curl_easy_setopt(request->easy, CURLOPT_HTTPHEADER, request->headers);
        curl_easy_setopt(request->easy, CURLOPT_TIMEOUT_MS, timeout_ms_);
        curl_easy_setopt(request->easy, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(request->easy, CURLOPT_POST, 0L);
        curl_easy_setopt(request->easy, CURLOPT_WRITEDATA, &request->response_body);

        enqueue_async_request(std::move(request));
    }

    void HttpClient::get_async(const std::string &path, const std::map<std::string, std::string> &custom_headers,
                               AsyncCallback callback)
    {
        std::string url = base_url_.empty() ? path : base_url_ + path;

        auto request = std::make_unique<AsyncRequest>();
        request->easy = curl_easy_init();
        if (!request->easy)
        {
            if (callback)
            {
                HttpResponse response;
                response.status_code = 0;
                response.error = "Failed to initialize CURL handle";
                callback(response);
            }
            return;
        }
        request->url = url;
        request->callback = std::move(callback);
        request->start = std::chrono::high_resolution_clock::now();
        request->headers = build_headers(&custom_headers);

        apply_common_options(request->easy);
        curl_easy_setopt(request->easy, CURLOPT_URL, request->url.c_str());
        curl_easy_setopt(request->easy, CURLOPT_HTTPHEADER, request->headers);
        curl_easy_setopt(request->easy, CURLOPT_TIMEOUT_MS, timeout_ms_);
        curl_easy_setopt(request->easy, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(request->easy, CURLOPT_POST, 0L);
        curl_easy_setopt(request->easy, CURLOPT_WRITEDATA, &request->response_body);

        enqueue_async_request(std::move(request));
    }

    void HttpClient::post_async(const std::string &path, const std::string &body, AsyncCallback callback)
    {
        std::string url = base_url_.empty() ? path : base_url_ + path;

        auto request = std::make_unique<AsyncRequest>();
        request->easy = curl_easy_init();
        if (!request->easy)
        {
            if (callback)
            {
                HttpResponse response;
                response.status_code = 0;
                response.error = "Failed to initialize CURL handle";
                callback(response);
            }
            return;
        }
        request->url = url;
        request->request_body = body;
        request->callback = std::move(callback);
        request->start = std::chrono::high_resolution_clock::now();
        request->headers = build_headers(nullptr);

        apply_common_options(request->easy);
        curl_easy_setopt(request->easy, CURLOPT_URL, request->url.c_str());
        curl_easy_setopt(request->easy, CURLOPT_HTTPHEADER, request->headers);
        curl_easy_setopt(request->easy, CURLOPT_TIMEOUT_MS, timeout_ms_);
        curl_easy_setopt(request->easy, CURLOPT_POST, 1L);
        curl_easy_setopt(request->easy, CURLOPT_POSTFIELDS, request->request_body.c_str());
        curl_easy_setopt(request->easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(request->request_body.size()));
        curl_easy_setopt(request->easy, CURLOPT_WRITEDATA, &request->response_body);

        enqueue_async_request(std::move(request));
    }

    void HttpClient::post_async(const std::string &path, const std::string &body,
                                const std::map<std::string, std::string> &custom_headers,
                                AsyncCallback callback)
    {
        std::string url = base_url_.empty() ? path : base_url_ + path;

        auto request = std::make_unique<AsyncRequest>();
        request->easy = curl_easy_init();
        if (!request->easy)
        {
            if (callback)
            {
                HttpResponse response;
                response.status_code = 0;
                response.error = "Failed to initialize CURL handle";
                callback(response);
            }
            return;
        }
        request->url = url;
        request->request_body = body;
        request->callback = std::move(callback);
        request->start = std::chrono::high_resolution_clock::now();
        request->headers = build_headers(&custom_headers);

        apply_common_options(request->easy);
        curl_easy_setopt(request->easy, CURLOPT_URL, request->url.c_str());
        curl_easy_setopt(request->easy, CURLOPT_HTTPHEADER, request->headers);
        curl_easy_setopt(request->easy, CURLOPT_TIMEOUT_MS, timeout_ms_);
        curl_easy_setopt(request->easy, CURLOPT_POST, 1L);
        curl_easy_setopt(request->easy, CURLOPT_POSTFIELDS, request->request_body.c_str());
        curl_easy_setopt(request->easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(request->request_body.size()));
        curl_easy_setopt(request->easy, CURLOPT_WRITEDATA, &request->response_body);

        enqueue_async_request(std::move(request));
    }

    size_t HttpClient::poll_async_internal(long timeout_ms)
    {
        if (!multi_)
        {
            return 0;
        }

        std::vector<std::pair<AsyncCallback, HttpResponse>> completed;

        int numfds = 0;
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            curl_multi_poll(multi_, nullptr, 0, timeout_ms, &numfds);

            int still_running = 0;
            curl_multi_perform(multi_, &still_running);

            int msgs_in_queue = 0;
            while (CURLMsg *msg = curl_multi_info_read(multi_, &msgs_in_queue))
            {
                if (msg->msg != CURLMSG_DONE)
                {
                    continue;
                }

                CURL *easy = msg->easy_handle;
                auto it = pending_async_requests_.find(easy);
                if (it == pending_async_requests_.end())
                {
                    curl_multi_remove_handle(multi_, easy);
                    curl_easy_cleanup(easy);
                    continue;
                }

                AsyncRequest *request = it->second.get();
                HttpResponse response;
                response.status_code = 0;
                response.body = request->response_body;

                auto end = std::chrono::high_resolution_clock::now();
                response.elapsed_ms = std::chrono::duration<double, std::milli>(end - request->start).count();

                if (msg->data.result != CURLE_OK)
                {
                    response.error = curl_easy_strerror(msg->data.result);
                }
                else
                {
                    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response.status_code);
                }

                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    total_requests_++;
                    total_latency_ms_ += response.elapsed_ms;
                    last_latency_ms_ = response.elapsed_ms;

                    long reused = 0;
                    curl_easy_getinfo(easy, CURLINFO_NUM_CONNECTS, &reused);
                    if (reused == 0)
                    {
                        reused_connections_++;
                    }
                }

                completed.emplace_back(request->callback, std::move(response));

                curl_multi_remove_handle(multi_, easy);
                if (request->headers)
                {
                    curl_slist_free_all(request->headers);
                }
                curl_easy_cleanup(easy);
                pending_async_requests_.erase(it);
            }
        }

        for (auto &entry : completed)
        {
            if (entry.first)
            {
                entry.first(entry.second);
            }
        }
        return completed.size();
    }

    void HttpClient::poll_async(long timeout_ms)
    {
        poll_async_internal(timeout_ms);
    }

    size_t HttpClient::pending_async() const
    {
        std::lock_guard<std::mutex> lock(async_mutex_);
        return pending_async_requests_.size();
    }

    void HttpClient::ensure_async_worker()
    {
        bool expected = false;
        if (!async_running_.compare_exchange_strong(expected, true))
        {
            return;
        }

        async_thread_ = std::thread([this]()
                                    {
            while (async_running_.load())
            {
                if (pending_async() == 0)
                {
                    std::unique_lock<std::mutex> lock(async_mutex_);
                    async_cv_.wait_for(lock, std::chrono::milliseconds(1));
                    continue;
                }
                size_t completed = poll_async_internal(0);
                if (completed == 0)
                {
                    long timeout_ms = 1;
                    curl_multi_timeout(multi_, &timeout_ms);
                    if (timeout_ms < 0 || timeout_ms > 1)
                    {
                        timeout_ms = 1;
                    }
                    poll_async_internal(timeout_ms);
                }
            } });
    }

    void HttpClient::stop_async_worker()
    {
        async_running_.store(false);
        async_cv_.notify_all();
        if (async_thread_.joinable())
        {
            async_thread_.join();
        }
    }

    // ============================================================
    // Connection Warming and Heartbeat
    // ============================================================

    bool HttpClient::warm_connection()
    {
        if (base_url_.empty())
        {
            return false;
        }

        // Hit a cheap endpoint to establish TCP/TLS
        auto response = get("/");
        if (response.ok() || response.status_code == 404)
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            connection_warm_ = true;
            return true;
        }
        return false;
    }

    void HttpClient::start_heartbeat(long interval_seconds)
    {
        if (heartbeat_running_.load())
        {
            return; // Already running
        }

        heartbeat_running_.store(true);
        heartbeat_thread_ = std::thread([this, interval_seconds]()
                                        {
            while (heartbeat_running_.load())
            {
                // Sleep in small increments to allow quick shutdown
                for (long i = 0; i < interval_seconds * 10 && heartbeat_running_.load(); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                if (!heartbeat_running_.load())
                {
                    break;
                }

                // Send a lightweight GET to keep connection alive
                std::lock_guard<std::mutex> lock(curl_mutex_);
                if (curl_ && !base_url_.empty())
                {
                    get("/");
                }
            } });
    }

    void HttpClient::stop_heartbeat()
    {
        heartbeat_running_.store(false);
        if (heartbeat_thread_.joinable())
        {
            heartbeat_thread_.join();
        }
    }

    bool HttpClient::is_heartbeat_running() const
    {
        return heartbeat_running_.load();
    }

    HttpClient::ConnectionStats HttpClient::get_stats() const
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ConnectionStats stats;
        stats.total_requests = total_requests_;
        stats.reused_connections = reused_connections_;
        stats.avg_latency_ms = total_requests_ > 0 ? total_latency_ms_ / total_requests_ : 0.0;
        stats.last_latency_ms = last_latency_ms_;
        stats.connection_warm = connection_warm_;
        return stats;
    }

} // namespace polymarket
