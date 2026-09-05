#include "plugins/sinks/http/sink.h"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "core/registry.h"
#include "plugins/sinks/http/endpoint.h"

namespace slurm_tracer {
namespace {

bool write_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// First token after the status line's leading "HTTP/1.x ", e.g. "204" out of
// "HTTP/1.1 204 No Content". 0 for anything that doesn't parse.
int parse_status_code(const std::string& status_line) {
    const size_t sp1 = status_line.find(' ');
    if (sp1 == std::string::npos)
        return 0;
    const size_t sp2 = status_line.find(' ', sp1 + 1);
    const std::string code = status_line.substr(sp1 + 1, sp2 - sp1 - 1);
    return std::atoi(code.c_str());
}

// Finds "name: value" in a (lowercased) header block, already known to
// contain no chunk of the body. Returns the trimmed value or "".
std::string header_value(const std::string& headers_lower, const std::string& name_lower) {
    const size_t pos = headers_lower.find(name_lower + ":");
    if (pos == std::string::npos)
        return {};
    size_t start = pos + name_lower.size() + 1;
    while (start < headers_lower.size() && headers_lower[start] == ' ')
        ++start;
    const size_t end = headers_lower.find("\r\n", start);
    return headers_lower.substr(start, end == std::string::npos ? end : end - start);
}

} // namespace

HttpSink::HttpSink(Options opt)
    : opt_(std::move(opt)) {
    worker_ = std::thread(&HttpSink::run, this);
}

HttpSink::~HttpSink() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void HttpSink::write(std::vector<Record> batch) {
    if (batch.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Drop the oldest, not the newest, same as stdout_json: under
        // sustained overload the records an operator is looking at right now
        // matter more than ones already stale.
        while (queue_.size() >= opt_.max_queued_batches) {
            queue_.pop_front();
            ++written_batches_;
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.push_back(std::move(batch));
        ++queued_batches_;
    }
    cv_.notify_one();
}

void HttpSink::flush() {
    std::unique_lock<std::mutex> lock(mu_);
    const uint64_t target = queued_batches_;
    drained_cv_.wait(lock, [&] { return written_batches_ >= target || stopping_; });
}

bool HttpSink::ensure_connected() {
    if (fd_ >= 0)
        return true;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(opt_.port);
    if (::getaddrinfo(opt_.host.c_str(), port_str.c_str(), &hints, &result) != 0)
        return false;

    int fd = -1;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;

        timeval tv{};
        tv.tv_sec = static_cast<time_t>(opt_.timeout.count() / 1000);
        tv.tv_usec = static_cast<suseconds_t>((opt_.timeout.count() % 1000) * 1000);
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(result);

    if (fd < 0)
        return false;
    fd_ = fd;
    return true;
}

void HttpSink::close_connection() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool HttpSink::post(const std::string& body) {
    if (!ensure_connected())
        return false;

    std::string request;
    request.reserve(body.size() + 192);
    request += "POST ";
    request += opt_.path;
    request += " HTTP/1.1\r\n";
    request += "Host: ";
    request += opt_.host;
    request += "\r\n";
    request += "Content-Type: application/x-ndjson\r\n";
    request += "Content-Length: ";
    request += std::to_string(body.size());
    request += "\r\n";
    request += "Connection: keep-alive\r\n\r\n";
    request += body;

    if (!write_all(fd_, request.data(), request.size())) {
        close_connection();
        return false;
    }

    // Read until the header block is fully in hand -- the status line is all
    // this sink actually needs, but the body (if any) has to be drained too,
    // or its bytes corrupt the next request's response on this connection.
    std::string buf;
    char chunk[4096];
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            close_connection();
            return false;
        }
        buf.append(chunk, static_cast<size_t>(n));
        header_end = buf.find("\r\n\r\n");
        // A well-behaved server's headers fit comfortably in a few KiB; past
        // that, something is wrong and there is no framing left to trust.
        if (header_end == std::string::npos && buf.size() > 64 * 1024) {
            close_connection();
            return false;
        }
    }

    const std::string status_line = buf.substr(0, buf.find("\r\n"));
    const int status = parse_status_code(status_line);
    const bool ok = status >= 200 && status < 300;

    const std::string headers_lower = lower(buf.substr(0, header_end));
    const bool chunked = headers_lower.find("transfer-encoding: chunked") != std::string::npos;
    const std::string content_length_str = header_value(headers_lower, "content-length");

    bool close_after = headers_lower.find("connection: close") != std::string::npos;

    if (chunked) {
        // Chunked framing is real work to parse correctly and no ingest
        // endpoint worth talking to needs it for a short ack; rather than
        // guess, just stop trusting this connection.
        close_after = true;
    } else if (!content_length_str.empty()) {
        size_t have = buf.size() - (header_end + 4);
        const size_t want =
            static_cast<size_t>(std::strtoul(content_length_str.c_str(), nullptr, 10));
        while (have < want) {
            const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                close_after = true;
                break;
            }
            have += static_cast<size_t>(n);
        }
    }
    // No Content-Length and not chunked: assume a zero-length body, which is
    // what every server that would otherwise ambiguously terminate on close
    // sends for a 2xx/204 ack in practice.

    if (close_after)
        close_connection();

    return ok;
}

void HttpSink::run() {
    for (;;) {
        std::vector<Record> batch;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return !queue_.empty() || stopping_; });
            if (queue_.empty()) {
                if (stopping_)
                    break;
                continue;
            }
            batch = std::move(queue_.front());
            queue_.pop_front();
        }

        std::string body;
        for (const Record& r : batch) {
            body += to_json(r);
            body += '\n';
        }

        // One retry against a fresh connection: a keep-alive connection the
        // peer quietly closed fails its first write for free, and is worth
        // one reconnect before the batch is counted as lost.
        if (!post(body)) {
            close_connection();
            if (!post(body))
                dropped_.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            ++written_batches_;
        }
        drained_cv_.notify_all();
    }

    close_connection();
    drained_cv_.notify_all();
}

// Called by the generated registry_manifest.cpp.
void register_http(Registries& r) {
    r.sinks.add("http", [](const ComponentConfig& config) -> std::unique_ptr<Sink> {
        const std::string endpoint = config.get("endpoint", "");
        const auto parsed = parse_http_endpoint(endpoint);
        if (!parsed) {
            std::cerr << "sink http: 'endpoint' is not a valid http:// URL (" << endpoint
                      << "); expected http://host[:port][/path]\n";
            return nullptr;
        }

        HttpSink::Options opt;
        opt.host = parsed->host;
        opt.port = parsed->port;
        opt.path = parsed->path;
        opt.max_queued_batches = static_cast<size_t>(config.get_uint("max_queued_batches", 1024));
        opt.timeout = config.get_duration("timeout", std::chrono::milliseconds(5000));
        return std::make_unique<HttpSink>(std::move(opt));
    });
}

} // namespace slurm_tracer
