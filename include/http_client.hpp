#pragma once

#include <string>
#include <optional>
#include <functional>
#include <map>
#include <atomic>
#include <thread>
#include <mutex>
#include <curl/curl.h>

namespace polymarket
{

    // HTTP response
    struct HttpResponse
    {
        long status_code;
        std::string body;
        std::string error;
        double elapsed_ms;

        bool ok() const { return status_code >= 200 && status_code < 300; }
    };

    // High-performance HTTP client using libcurl
    class HttpClient
    {
    public:
        HttpClient();
        ~HttpClient();

        // Disable copy
        HttpClient(const HttpClient &) = delete;
        HttpClient &operator=(const HttpClient &) = delete;

        // Enable move
        HttpClient(HttpClient &&other) noexcept;
        HttpClient &operator=(HttpClient &&other) noexcept;

        // Configuration
        void set_timeout_ms(long timeout_ms);
        void set_base_url(const std::string &base_url);
        void add_header(const std::string &header);
        void set_proxy(const std::string &proxy_url); // e.g., "http://user:pass@proxy.example.com:8080"
        void set_user_agent(const std::string &user_agent);
        void set_dns_cache_timeout(long seconds);  // DNS cache TTL (default: 60s)
        void set_keepalive_interval(long seconds); // TCP keepalive probe interval

        // HTTP methods
        HttpResponse get(const std::string &path);
        HttpResponse get(const std::string &path, const std::map<std::string, std::string> &custom_headers);
        HttpResponse post(const std::string &path, const std::string &body);
        HttpResponse post(const std::string &path, const std::string &body, const std::map<std::string, std::string> &custom_headers);
        HttpResponse del(const std::string &path, const std::string &body = "");
        HttpResponse del(const std::string &path, const std::string &body, const std::map<std::string, std::string> &custom_headers);

        // Connection warming and keep-alive
        bool warm_connection();                           // Pre-warm TCP/TLS with a cheap GET
        void start_heartbeat(long interval_seconds = 25); // Start background heartbeat to keep connection alive
        void stop_heartbeat();                            // Stop background heartbeat
        bool is_heartbeat_running() const;

        // Connection stats
        struct ConnectionStats
        {
            long total_requests;
            long reused_connections;
            double avg_latency_ms;
            double last_latency_ms;
            bool connection_warm;
        };
        ConnectionStats get_stats() const;

    private:
        CURL *curl_;
        struct curl_slist *headers_;
        std::string base_url_;
        std::string proxy_url_;
        long timeout_ms_;
        long dns_cache_timeout_;
        long keepalive_interval_;

        // Heartbeat thread
        std::atomic<bool> heartbeat_running_;
        std::thread heartbeat_thread_;
        mutable std::mutex curl_mutex_;

        // Connection stats
        mutable std::mutex stats_mutex_;
        long total_requests_;
        long reused_connections_;
        double total_latency_ms_;
        double last_latency_ms_;
        bool connection_warm_;

        void init();
        void cleanup();
        HttpResponse perform(const std::string &url);

        static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
    };

    // Global initialization (call once at startup)
    void http_global_init();
    void http_global_cleanup();

} // namespace polymarket
