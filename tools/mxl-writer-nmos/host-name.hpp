// The DNS name a pod is reached at, which the Node advertises its APIs under.
// Links neither libmxl nor NvNmos, so senders-test.cpp covers it.

#pragma once

#include <arpa/inet.h>
#include <istream>
#include <optional>
#include <sstream>
#include <string>

namespace host_name
{
    // <address>.<namespace>.pod.<cluster domain>, with the address's dots and
    // colons as dashes: the A record CoreDNS serves for a pod where its
    // kubernetes plugin runs with "pods insecure" or "pods verified". The
    // cluster domain is what follows "<namespace>.svc." in the resolv.conf
    // search list, of which the resolver reads the last search line. Empty
    // where that line has no such entry.
    inline std::string podDnsName(std::string const& address, std::string const& ns, std::istream& resolvConf)
    {
        auto const prefix = ns + ".svc.";
        auto line = std::string{};
        auto search = std::string{};
        while (std::getline(resolvConf, line))
        {
            auto words = std::istringstream{line};
            auto word = std::string{};
            if (words >> word && word == "search")
            {
                search = line;
            }
        }
        auto words = std::istringstream{search};
        auto word = std::string{};
        words >> word;
        while (words >> word)
        {
            if (!word.starts_with(prefix))
            {
                continue;
            }
            auto domain = word.substr(prefix.size());
            if (domain.ends_with('.'))
            {
                domain.pop_back();
            }
            if (domain.empty())
            {
                continue;
            }
            auto name = address;
            for (auto& c : name)
            {
                if (c == '.' || c == ':')
                {
                    c = '-';
                }
            }
            return name + "." + ns + ".pod." + domain;
        }
        return {};
    }

    // The host name the Node is given, or nothing with the reason in error:
    // explicit where set, else the pod's DNS name. NvNmos lists every API
    // under the host name first, so it has to be a DNS name a controller
    // resolves. Neither fallback is one: NvNmos's default is the pod's own
    // host name under a guessed domain, and the address as host name lists
    // every href twice. NvNmos appends a domain to a name without a dot.
    inline std::optional<std::string> nodeHostName(std::string const& explicitName, std::string const& address, std::string const& ns,
        std::istream& resolvConf, std::string& error)
    {
        if (!explicitName.empty())
        {
            auto buf = in6_addr{};
            if (::inet_pton(AF_INET, explicitName.c_str(), &buf) == 1 || ::inet_pton(AF_INET6, explicitName.c_str(), &buf) == 1)
            {
                error = "NMOS_HOST_NAME is an address, not a DNS name";
                return std::nullopt;
            }
            if (explicitName.find('.') == std::string::npos)
            {
                error = "NMOS_HOST_NAME has no domain, and NvNmos would append one to it";
                return std::nullopt;
            }
            return explicitName;
        }
        if (ns.empty())
        {
            error = "NMOS_HOST_NAME or NMOS_POD_NAMESPACE is required: the Node's host name has to be a DNS name controllers resolve";
            return std::nullopt;
        }
        auto name = podDnsName(address, ns, resolvConf);
        if (name.empty())
        {
            error = "NMOS_POD_NAMESPACE is " + ns + ", but /etc/resolv.conf searches no " + ns + ".svc. domain";
            return std::nullopt;
        }
        return name;
    }
}
