// The HTTP side of mxl-writer-status: one thread polling every connection, so
// a client that connects and sends nothing, or reads its answer slowly, holds
// up nobody else. A request is served once its header is complete; one whose
// header does not complete within requestTimeout is dropped, and one that
// grows past MaxRequestBytes is refused. Event streams are fed from the same
// loop, so no thread outlives the server.

#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fmt/format.h>
#include <netinet/in.h>

namespace http
{
    // A request header larger than this is refused with 431. A browser
    // sends every cookie of the parent domain along, which alone can pass
    // 8 KiB, and a proxy in front adds its own forwarding headers. Envoy
    // refuses a header above 60 KiB by default, so this leaves 4 KiB for
    // what it adds; at most maxConnections buffers this size are held.
    constexpr std::size_t MaxRequestBytes = 64 * 1024;

    struct Request
    {
        enum class Outcome
        {
            Incomplete,
            TooLarge,
            Bad,
            Complete,
        };
        Outcome outcome = Outcome::Incomplete;
        std::string method;
        // The request target without its query.
        std::string path;
    };

    // Reads the request line from what a client has sent so far. Only the
    // method and the path are of interest; the rest of the header is skipped,
    // but it has to end before the request is served, so a client cannot be
    // answered halfway through sending it.
    inline Request parseRequest(std::string_view received)
    {
        auto request = Request{};
        auto const end = received.find("\r\n\r\n");
        if (end == std::string_view::npos)
        {
            request.outcome = received.size() >= MaxRequestBytes ? Request::Outcome::TooLarge : Request::Outcome::Incomplete;
            return request;
        }
        if (end + 4 > MaxRequestBytes)
        {
            request.outcome = Request::Outcome::TooLarge;
            return request;
        }
        auto const line = received.substr(0, received.find("\r\n"));
        auto const firstSpace = line.find(' ');
        auto const secondSpace = firstSpace == std::string_view::npos ? std::string_view::npos : line.find(' ', firstSpace + 1);
        if (firstSpace == 0 || secondSpace == std::string_view::npos || secondSpace == firstSpace + 1 ||
            !line.substr(secondSpace + 1).starts_with("HTTP/1."))
        {
            request.outcome = Request::Outcome::Bad;
            return request;
        }
        auto const target = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        if (!target.starts_with('/'))
        {
            request.outcome = Request::Outcome::Bad;
            return request;
        }
        request.outcome = Request::Outcome::Complete;
        request.method = std::string{line.substr(0, firstSpace)};
        request.path = std::string{target.substr(0, target.find('?'))};
        return request;
    }

    inline char const* reason(int code)
    {
        switch (code)
        {
            case 200: return "OK";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 431: return "Request Header Fields Too Large";
            case 503: return "Service Unavailable";
            default:  return "Error";
        }
    }

    struct Response
    {
        int code = 200;
        std::string type = "text/plain; charset=utf-8";
        std::string body{};
        // Answered with an event stream instead: the header, then a batch
        // from Server::tick every StreamInterval.
        bool stream = false;
    };

    inline std::string encode(Response const& response)
    {
        if (response.stream)
        {
            return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                   "Connection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\n";
        }
        return fmt::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            response.code,
            reason(response.code),
            response.type,
            response.body.size(),
            response.body);
    }

    struct Server
    {
        using Clock = std::chrono::steady_clock;

        // Answers a GET for a path.
        std::function<Response(std::string const& path)> route;
        // The next batch for one stream. The cursors are the stream's own and
        // empty on its first call.
        std::function<std::string(std::vector<std::uint64_t>& cursors)> tick;

        std::chrono::milliseconds requestTimeout{5000};
        std::chrono::milliseconds responseTimeout{10000};
        std::chrono::milliseconds streamInterval{50};
        std::size_t maxConnections = 64;
        std::size_t maxStreams = 8;
        // A stream whose viewer has not taken this much is dropped rather
        // than buffered without end.
        std::size_t maxStreamBacklog = 1 << 20;

        int listener = -1;

        // Binds the port, 0 for any. Returns the port bound, or none.
        std::optional<std::uint16_t> listen(std::uint16_t port, bool loopbackOnly = false)
        {
            listener = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (listener < 0)
            {
                return std::nullopt;
            }
            auto const one = 1;
            ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            auto addr = sockaddr_in{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
            addr.sin_port = htons(port);
            if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listener, 64) != 0)
            {
                ::close(listener);
                listener = -1;
                return std::nullopt;
            }
            auto bound = sockaddr_in{};
            auto length = socklen_t{sizeof(bound)};
            ::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &length);
            return ntohs(bound.sin_port);
        }

        // Serves until stop is set; at most streamInterval, or a second
        // without streams, after it is.
        void run(std::atomic<bool> const& stop)
        {
            auto conns = std::vector<Connection>{};
            auto nextTick = Clock::now();
            auto fds = std::vector<pollfd>{};
            while (!stop)
            {
                auto streams = std::size_t{0};
                fds.clear();
                fds.push_back({.fd = listener, .events = POLLIN, .revents = 0});
                for (auto const& c : conns)
                {
                    streams += c.stream ? 1 : 0;
                    auto events = short{POLLIN};
                    if (!c.out.empty())
                    {
                        events |= POLLOUT;
                    }
                    fds.push_back({.fd = c.fd, .events = events, .revents = 0});
                }
                auto const now = Clock::now();
                auto wait = std::chrono::milliseconds{1000};
                if (streams != 0)
                {
                    wait = std::max(std::chrono::milliseconds{0}, std::chrono::duration_cast<std::chrono::milliseconds>(nextTick - now));
                }
                for (auto const& c : conns)
                {
                    if (!c.stream)
                    {
                        wait = std::min(wait,
                            std::max(std::chrono::milliseconds{0}, std::chrono::duration_cast<std::chrono::milliseconds>(c.deadline - now)));
                    }
                }
                if (::poll(fds.data(), fds.size(), static_cast<int>(wait.count())) < 0 && errno != EINTR)
                {
                    break;
                }

                for (std::size_t i = 0; i < conns.size(); ++i)
                {
                    auto& c = conns[i];
                    auto const revents = fds[i + 1].revents;
                    if ((revents & (POLLERR | POLLNVAL)) != 0)
                    {
                        c.done = true;
                        continue;
                    }
                    if ((revents & (POLLIN | POLLHUP)) != 0)
                    {
                        receive(c, streams);
                    }
                    if (!c.done && (revents & POLLOUT) != 0)
                    {
                        transmit(c);
                    }
                }

                auto const after = Clock::now();
                if (streams != 0 && after >= nextTick)
                {
                    for (auto& c : conns)
                    {
                        if (c.stream && !c.done)
                        {
                            if (c.out.size() > maxStreamBacklog)
                            {
                                c.done = true;
                                continue;
                            }
                            c.out += "data: " + tick(c.cursors) + "\n\n";
                            transmit(c);
                        }
                    }
                    nextTick = after + streamInterval;
                }
                else if (streams == 0)
                {
                    nextTick = after;
                }

                for (auto& c : conns)
                {
                    if (!c.stream && after >= c.deadline)
                    {
                        c.done = true;
                    }
                }
                std::erase_if(conns,
                    [](Connection& c)
                    {
                        if (c.done)
                        {
                            ::close(c.fd);
                        }
                        return c.done;
                    });

                if ((fds[0].revents & POLLIN) != 0)
                {
                    accept(conns);
                }
            }
            for (auto const& c : conns)
            {
                ::close(c.fd);
            }
            ::close(listener);
            listener = -1;
        }

    private:
        struct Connection
        {
            int fd = -1;
            std::string in{};
            std::string out{};
            Clock::time_point deadline{};
            bool answered = false;
            bool stream = false;
            bool done = false;
            std::vector<std::uint64_t> cursors{};
        };

        void accept(std::vector<Connection>& conns)
        {
            while (true)
            {
                auto const fd = ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0)
                {
                    return;
                }
                if (conns.size() >= maxConnections)
                {
                    ::close(fd);
                    continue;
                }
                conns.push_back({.fd = fd, .deadline = Clock::now() + requestTimeout});
            }
        }

        void answer(Connection& c, Response const& response)
        {
            c.answered = true;
            c.stream = response.stream;
            c.out = encode(response);
            c.deadline = Clock::now() + responseTimeout;
            if (c.stream)
            {
                c.out += "data: " + tick(c.cursors) + "\n\n";
            }
            transmit(c);
        }

        void receive(Connection& c, std::size_t& streams)
        {
            char buffer[4096];
            while (true)
            {
                auto const n = ::recv(c.fd, buffer, sizeof(buffer), MSG_DONTWAIT);
                if (n == 0)
                {
                    // The client closed: nobody is left to answer.
                    c.done = true;
                    return;
                }
                if (n < 0)
                {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    {
                        c.done = true;
                    }
                    return;
                }
                if (c.answered)
                {
                    // Anything sent after the request is not read.
                    continue;
                }
                c.in.append(buffer, static_cast<std::size_t>(n));
                auto const request = parseRequest(c.in);
                switch (request.outcome)
                {
                    case Request::Outcome::Incomplete: continue;
                    case Request::Outcome::TooLarge:   answer(c, {.code = 431, .body = "request header too large\n"}); return;
                    case Request::Outcome::Bad:        answer(c, {.code = 400, .body = "bad request\n"}); return;
                    case Request::Outcome::Complete:   break;
                }
                if (request.method != "GET")
                {
                    answer(c, {.code = 405, .body = "only GET\n"});
                    return;
                }
                auto response = route(request.path);
                if (response.stream && streams >= maxStreams)
                {
                    response = {.code = 503, .body = fmt::format("at most {} streams at once\n", maxStreams)};
                }
                streams += response.stream ? 1 : 0;
                answer(c, response);
                return;
            }
        }

        // Sends what the socket takes now; the rest waits for POLLOUT. A
        // plain response closes once all of it is out.
        void transmit(Connection& c)
        {
            while (!c.out.empty())
            {
                auto const n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
                if (n < 0)
                {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    {
                        c.done = true;
                    }
                    return;
                }
                c.out.erase(0, static_cast<std::size_t>(n));
            }
            if (c.answered && !c.stream)
            {
                c.done = true;
            }
        }
    };
}
