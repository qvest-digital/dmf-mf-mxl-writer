// The writers' metrics as one Prometheus target. Each writer process serves
// its own flow's counts on loopback; the Node fetches them on every scrape and
// serves them merged, so a booking has one metrics port whichever of its
// flows are being written.

#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace metrics
{
    // Joins Prometheus text expositions into one. The same metric family in
    // two of them would be two HELP and TYPE lines for one name, which a
    // scraper refuses, so each family is written once, with its header from
    // the first exposition that has it and the samples of all of them, in the
    // order the families first appear. The writers label every sample with
    // their flow, so no two samples collide.
    inline std::string merge(std::vector<std::string> const& texts)
    {
        struct Family
        {
            std::string header{};
            std::string samples{};
            std::size_t owner = 0;
        };
        auto order = std::vector<std::string>{};
        auto families = std::map<std::string, Family>{};
        for (std::size_t t = 0; t < texts.size(); ++t)
        {
            auto current = std::string{};
            auto text = std::string_view{texts[t]};
            while (!text.empty())
            {
                auto const end = text.find('\n');
                auto const line = text.substr(0, end);
                text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
                if (line.empty())
                {
                    continue;
                }
                auto const header = line.starts_with("# HELP ") || line.starts_with("# TYPE ");
                if (line.starts_with('#') && !header)
                {
                    continue;
                }
                auto const name = header ? line.substr(7, line.find(' ', 7) - 7) : line.substr(0, line.find_first_of("{ "));
                // A sample belongs to the family whose header precedes it,
                // which covers a histogram's _bucket, _sum and _count.
                if (header || current.empty())
                {
                    current = std::string{name};
                }
                auto [it, added] = families.try_emplace(current, Family{.owner = t});
                if (added)
                {
                    order.push_back(current);
                }
                auto& family = it->second;
                if (!header)
                {
                    family.samples.append(line).push_back('\n');
                }
                else if (family.owner == t)
                {
                    family.header.append(line).push_back('\n');
                }
            }
        }
        auto out = std::string{};
        for (auto const& name : order)
        {
            out += families[name].header;
            out += families[name].samples;
        }
        return out;
    }

    // The body of GET /metrics from 127.0.0.1:port, or nothing where nothing
    // answers 200 within timeout: a writer that is stopped, starting or stuck.
    inline std::optional<std::string> fetch(std::uint16_t port, std::chrono::milliseconds timeout)
    {
        auto const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
        {
            return std::nullopt;
        }
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        auto wait = [&](short events)
        {
            auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            auto pfd = pollfd{.fd = fd, .events = events, .revents = 0};
            return left.count() > 0 && ::poll(&pfd, 1, static_cast<int>(left.count())) == 1 && (pfd.revents & (POLLERR | POLLNVAL)) == 0;
        };
        auto addr = sockaddr_in{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        auto response = std::string{};
        auto ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 || (errno == EINPROGRESS && wait(POLLOUT));
        auto error = 0;
        auto length = socklen_t{sizeof(error)};
        ok = ok && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
        constexpr std::string_view Request = "GET /metrics HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        ok = ok && ::send(fd, Request.data(), Request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(Request.size());
        // The writer closes once it has answered.
        while (ok)
        {
            if (!wait(POLLIN))
            {
                ok = false;
                break;
            }
            char buffer[16384];
            auto const n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0)
            {
                ok = n == 0;
                break;
            }
            response.append(buffer, static_cast<std::size_t>(n));
        }
        ::close(fd);
        auto const body = response.find("\r\n\r\n");
        if (!ok || !response.starts_with("HTTP/1.1 200 ") || body == std::string::npos)
        {
            return std::nullopt;
        }
        return response.substr(body + 4);
    }
}
