// mxl-writer-nmos: mxl-gst-testsrc as an NMOS Node.
//
// Takes mxl-gst-testsrc's own command line and runs the test source once per
// flow, so each flow is written by a process of its own. The Node registers
// over IS-04 and serves an IS-05 Connection API with one BCP-007-03 MXL Sender
// per flow, through NvNmos. An activation that clears master_enable stops that
// flow's process, which releases the flow; one that sets it starts the process
// again. Every Sender starts active.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <string>
#include <thread>
#include <vector>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <nvnmos.h>
#include "host-name.hpp"
#include "repeats.hpp"
#include "senders.hpp"

extern char** environ;

namespace
{
    constexpr char const* TestSource = "/usr/bin/mxl-gst-testsrc";

    std::atomic<bool> stopping{false};

    void onSignal(int)
    {
        stopping = true;
    }

    void log(char const* format, auto... args)
    {
        std::fprintf(stderr, "[mxl-writer-nmos] ");
        std::fprintf(stderr, format, args...);
        std::fprintf(stderr, "\n");
    }

    std::string env(char const* name, char const* fallback = "")
    {
        auto const* value = std::getenv(name);
        return value != nullptr && *value != '\0' ? value : fallback;
    }

    // A port from the environment, 0 where unset, or nothing where the value
    // is not a port.
    std::optional<unsigned int> envPort(char const* name, char const* fallback = "0")
    {
        auto const text = env(name, fallback);
        auto value = 0UL;
        for (auto const c : text)
        {
            if (c < '0' || c > '9' || (value = value * 10 + static_cast<unsigned long>(c - '0')) > 65535)
            {
                return std::nullopt;
            }
        }
        return static_cast<unsigned int>(value);
    }

    struct Writer
    {
        senders::Flow flow;
        std::vector<std::string> args;
        std::string transportFile;
        // What the Sender's master_enable asks for. Set by the activation
        // callback, which NvNmos calls holding its model lock, so the process
        // itself is started and stopped by the main loop.
        std::atomic<bool> wanted{true};
        pid_t pid = 0;
    };

    std::vector<std::unique_ptr<Writer>> writers;
    std::string domain;
    std::string domainId;
    std::string groupHint;

    // NvNmos calls this once nmos-cpp has applied an IS-05 activation to its
    // model. A false return is logged by NvNmos as a failed activation and
    // changes nothing else: /active already reports the new state.
    bool onActivated(NvNmosNodeServer*, NvNmosSide side, char const* name, char const* transportFile)
    {
        if (side != NVNMOS_SIDE_SENDER || name == nullptr)
        {
            return false;
        }
        for (auto& writer : writers)
        {
            if (writer->flow.name != name)
            {
                continue;
            }
            if (transportFile == nullptr)
            {
                writer->wanted = false;
                log("%s sender deactivated: its writer of flow %s is to stop", name, writer->flow.flowId.c_str());
                return true;
            }
            // A guard only: IS-05 constrains mxl_flow_id to this one flow,
            // so a request naming another is refused before it gets here.
            auto const definition = senders::parseObject(transportFile);
            auto const id = definition ? senders::textField(*definition, "id") : std::string{};
            if (id != writer->flow.flowId)
            {
                log("%s sender activated for flow %s, which is not the flow it writes (%s)", name, id.c_str(), writer->flow.flowId.c_str());
                return false;
            }
            writer->wanted = true;
            log("%s sender activated: its writer of flow %s in domain %s is to run", name, id.c_str(), domainId.c_str());
            return true;
        }
        log("activation for unknown %s %s", side == NVNMOS_SIDE_SENDER ? "sender" : "receiver", name);
        return false;
    }

    // NvNmos repeats a failure to find or reach a registry several times a
    // second; each distinct line is printed at most once a minute, with a
    // count of the ones held back since its last print, and a line that
    // stops repeating gets its last count once it has been quiet a minute.
    std::mutex logMutex;
    repeats::Limiter logRepeats{60'000'000'000LL};

    std::int64_t steadyNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void onLog(NvNmosNodeServer*, char const* categories, int level, char const* message)
    {
        auto const* cats = categories ? categories : "";
        auto line = "[mxl-writer-nmos] nmos[" + std::to_string(level) + (*cats ? " " : "") + cats + "] " + (message ? message : "");
        auto heldBack = std::uint64_t{0};
        {
            auto const lock = std::lock_guard{logMutex};
            if (!logRepeats.admit(line, steadyNs(), heldBack))
            {
                return;
            }
        }
        if (heldBack > 0)
        {
            std::fprintf(stderr, "%s (%llu more like it since the last print)\n", line.c_str(), static_cast<unsigned long long>(heldBack));
        }
        else
        {
            std::fprintf(stderr, "%s\n", line.c_str());
        }
    }

    // The last count of every line that stopped repeating.
    void logStopped()
    {
        auto const now = steadyNs();
        auto stopped = std::vector<repeats::Limiter::Summary>{};
        {
            auto const lock = std::lock_guard{logMutex};
            stopped = logRepeats.stopped(now);
        }
        for (auto const& summary : stopped)
        {
            std::fprintf(stderr, "%s (%llu more like it since the last print, the last %lld s ago)\n", summary.line.c_str(),
                static_cast<unsigned long long>(summary.heldBack), static_cast<long long>((now - summary.lastSeenNs) / 1'000'000'000LL));
        }
    }

    bool spawn(Writer& writer)
    {
        auto argv = std::vector<char*>{};
        argv.push_back(const_cast<char*>(TestSource));
        for (auto& arg : writer.args)
        {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        auto const rc = ::posix_spawn(&writer.pid, TestSource, nullptr, nullptr, argv.data(), environ);
        if (rc != 0)
        {
            writer.pid = 0;
            log("cannot start the %s writer: %s", writer.flow.name.c_str(), std::strerror(rc));
            return false;
        }
        log("%s writer started (pid %d) for flow %s", writer.flow.name.c_str(), writer.pid, writer.flow.flowId.c_str());
        return true;
    }

    // Asks the writers to stop, as a pod shutdown does, and waits for them to
    // release their flows against one deadline; one still running after 10 s
    // is killed.
    void stop(std::vector<Writer*> const& writers)
    {
        for (auto* writer : writers)
        {
            if (writer->pid != 0)
            {
                ::kill(writer->pid, SIGTERM);
            }
        }
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        for (;;)
        {
            auto running = false;
            for (auto* writer : writers)
            {
                auto status = 0;
                if (writer->pid != 0 && ::waitpid(writer->pid, &status, WNOHANG) == writer->pid)
                {
                    log("%s writer stopped", writer->flow.name.c_str());
                    writer->pid = 0;
                }
                running = running || writer->pid != 0;
            }
            if (!running)
            {
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                for (auto* writer : writers)
                {
                    if (writer->pid != 0)
                    {
                        log("the %s writer did not stop within 10 s, killing it", writer->flow.name.c_str());
                        ::kill(writer->pid, SIGKILL);
                        ::waitpid(writer->pid, nullptr, 0);
                        writer->pid = 0;
                    }
                }
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }

    struct Node
    {
        NvNmosNodeServer server{};
        std::string hostAddress;
        // Advertised before the address.
        std::string hostName;
        std::string seed;
        std::string label;
        std::string description;
        std::string dnsDomain;
        std::string registryHost;
        std::string systemHost;
        unsigned int httpPort = 0;
        unsigned int registryPort = 0;
        unsigned int systemPort = 0;
        bool running = false;
    };

    // Starts the Node, or returns false with the reason in error; retry is
    // set where the domain has no readable identity yet, which the platform
    // may still be about to write.
    bool startNode(Node& node, std::string& error, bool& retry)
    {
        retry = false;
        auto id = senders::domainId(domain, error);
        if (!id)
        {
            retry = true;
            return false;
        }
        domainId = *id;

        auto configs = std::vector<NvNmosSenderConfig>{};
        for (auto& writer : writers)
        {
            writer->transportFile = senders::transportFile(writer->flow, groupHint, node.label, domainId);
            auto config = NvNmosSenderConfig{};
            config.transport = NVNMOS_TRANSPORT_MXL;
            config.transport_file = writer->transportFile.c_str();
            configs.push_back(config);
        }

        char const* hostAddresses[] = {node.hostAddress.c_str()};
        char const* functions[] = {"Test pattern generator"};
        auto asset = NvNmosAssetConfig{};
        asset.manufacturer = "DMF";
        asset.product = "dmf-mf-mxl-writer";
        asset.instance_id = node.seed.c_str();
        asset.functions = functions;
        asset.num_functions = 1;

        auto services = NvNmosNetworkServicesConfig{};
        services.domain = node.dnsDomain.empty() ? nullptr : node.dnsDomain.c_str();
        services.registration_address = node.registryHost.empty() ? nullptr : node.registryHost.c_str();
        services.registration_port = node.registryPort;
        services.system_address = node.systemHost.empty() ? nullptr : node.systemHost.c_str();
        services.system_port = node.systemPort;

        auto config = NvNmosNodeConfig{};
        // NvNmos advertises every API under the host name first, then the
        // addresses, so the host name has to be a DNS name controllers resolve.
        config.host_name = node.hostName.c_str();
        config.host_addresses = hostAddresses;
        config.num_host_addresses = 1;
        config.http_port = node.httpPort;
        config.label = node.label.c_str();
        config.description = node.description.c_str();
        config.asset_tags = &asset;
        config.seed = node.seed.c_str();
        config.senders = configs.data();
        config.num_senders = static_cast<unsigned int>(configs.size());
        config.connection_activated = &onActivated;
        config.log_callback = &onLog;
        config.log_level = NVNMOS_LOG_WARNING;
        config.network_services = &services;

        if (!create_nmos_node_server(&config, &node.server))
        {
            error = "the NMOS Node server did not start";
            return false;
        }
        node.running = true;

        // The writers run without a controller having asked, so /active says
        // so for every Sender still wanted.
        for (auto& writer : writers)
        {
            if (writer->wanted && !nmos_connection_activate(&node.server, NVNMOS_SIDE_SENDER, writer->flow.name.c_str(), writer->transportFile.c_str()))
            {
                log("could not report the %s sender active", writer->flow.name.c_str());
            }
        }
        log("node up at %s:%u, %zu sender(s) in domain %s (%s)", node.hostAddress.c_str(), node.httpPort, writers.size(), domainId.c_str(),
            domain.c_str());
        return true;
    }

}

int main(int argc, char** argv)
{
    std::signal(SIGINT, &onSignal);
    std::signal(SIGTERM, &onSignal);

    auto error = std::string{};
    auto const split = senders::splitArgs(std::vector<std::string>{argv + 1, argv + argc}, error);
    if (!split)
    {
        log("%s", error.c_str());
        return 2;
    }
    domain = split->domain;
    groupHint = split->groupHint;
    for (auto const& [name, path, args] : {std::tuple{"video", split->videoDefinition, split->video},
             std::tuple{"audio", split->audioDefinition, split->audio}})
    {
        if (path.empty())
        {
            continue;
        }
        auto flow = senders::readFlow(name, path, error);
        if (!flow)
        {
            log("%s", error.c_str());
            return 2;
        }
        auto writer = std::make_unique<Writer>();
        writer->flow = std::move(*flow);
        writer->args = args;
        writers.push_back(std::move(writer));
    }

    auto node = Node{};
    node.hostAddress = env("NMOS_HOST_ADDRESS");
    node.seed = env("NMOS_SEED");
    node.label = env("NMOS_LABEL", "MXL test writer");
    node.description = env("NMOS_DESCRIPTION", "Test patterns written into MXL flows");
    node.dnsDomain = env("NMOS_DNS_DOMAIN");
    node.registryHost = env("NMOS_REGISTRY_HOST");
    // nmos-cpp serves the IS-09 System API beside its registry, so a registry
    // named by address is the System API too unless told apart.
    node.systemHost = env("NMOS_SYSTEM_HOST", node.registryHost.c_str());
    for (auto const& [name, fallback, port] : {std::tuple{"NMOS_HTTP_PORT", std::string{"0"}, &node.httpPort},
             std::tuple{"NMOS_REGISTRY_PORT", std::string{"0"}, &node.registryPort},
             std::tuple{"NMOS_SYSTEM_PORT", env("NMOS_REGISTRY_PORT", "0"), &node.systemPort}})
    {
        auto const value = envPort(name, fallback.c_str());
        if (!value)
        {
            log("%s='%s' is not a port", name, env(name).c_str());
            return 2;
        }
        *port = *value;
    }
    if (node.hostAddress.empty())
    {
        log("NMOS_HOST_ADDRESS is required: the address the Node's APIs are reached at");
        return 2;
    }
    if (node.seed.empty())
    {
        // Resource ids derive from the seed, so without one they change on
        // every restart and a controller's connections point at resources
        // that no longer exist.
        log("NMOS_SEED is required: the Node's resource ids derive from it");
        return 2;
    }
    auto resolvConf = std::ifstream{"/etc/resolv.conf"};
    auto hostNameError = std::string{};
    auto hostName = host_name::nodeHostName(env("NMOS_HOST_NAME"), node.hostAddress, env("NMOS_POD_NAMESPACE"), resolvConf, hostNameError);
    if (!hostName)
    {
        log("%s", hostNameError.c_str());
        return 2;
    }
    node.hostName = *hostName;

    // The flows are written whether or not the Node is up yet, as without
    // NMOS.
    auto failed = false;
    auto nextTry = std::chrono::steady_clock::now();
    while (!stopping && !failed)
    {
        if (!node.running && std::chrono::steady_clock::now() >= nextTry)
        {
            auto retry = false;
            if (!startNode(node, error, retry))
            {
                if (!retry)
                {
                    log("%s", error.c_str());
                    failed = true;
                    break;
                }
                log("%s; trying again in 10 s", error.c_str());
                nextTry = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            }
        }
        // As the container's PID 1 this process also inherits every orphan
        // a writer leaves, so every exited child is reaped here, not only
        // the writers.
        auto status = 0;
        for (auto pid = ::waitpid(-1, &status, WNOHANG); pid > 0 && !failed; pid = ::waitpid(-1, &status, WNOHANG))
        {
            for (auto& writer : writers)
            {
                if (writer->pid != pid)
                {
                    continue;
                }
                writer->pid = 0;
                if (writer->wanted)
                {
                    // Exiting takes the pod's restart policy with it, which
                    // is what the test source alone gets when it stops.
                    log("the %s writer exited (status %d) while its sender is active", writer->flow.name.c_str(),
                        WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
                    failed = true;
                }
            }
        }
        for (auto& writer : writers)
        {
            if (failed)
            {
                break;
            }
            if (writer->wanted && writer->pid == 0 && !spawn(*writer))
            {
                failed = true;
            }
            else if (!writer->wanted && writer->pid != 0)
            {
                stop({writer.get()});
            }
        }
        logStopped();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    if (node.running)
    {
        destroy_nmos_node_server(&node.server);
    }
    auto all = std::vector<Writer*>{};
    for (auto& writer : writers)
    {
        all.push_back(writer.get());
    }
    stop(all);
    return failed ? 1 : 0;
}
