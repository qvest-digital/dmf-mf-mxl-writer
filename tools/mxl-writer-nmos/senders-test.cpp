// Tests of what mxl-writer-nmos works out before it starts a writer or the
// Node. Built and run by the image build, so an image that would advertise a
// flow it does not write, or run a writer with another flow's options, is
// never published.

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include "host-name.hpp"
#include "metrics.hpp"
#include "repeats.hpp"
#include "senders.hpp"
#include "server.hpp"

namespace
{
    int failures = 0;

    void check(bool ok, char const* what)
    {
        if (!ok)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    bool has(std::vector<std::string> const& args, std::string const& flag, std::string const& value)
    {
        for (std::size_t i = 0; i + 1 < args.size(); ++i)
        {
            if (args[i] == flag && args[i + 1] == value)
            {
                return true;
            }
        }
        return false;
    }

    bool hasFlag(std::vector<std::string> const& args, std::string const& flag)
    {
        return std::find(args.begin(), args.end(), flag) != args.end();
    }

    // The command line the mxl-writer chart renders for a video and an
    // audio flow.
    std::vector<std::string> const Chart = {"-d", "/run/mxl/domain-1", "-v", "/etc/flow-config/video-flow.json", "-p", "ball", "-t", "TEXT",
        "--video-offset", "2", "-a", "/etc/flow-config/audio-flow.json", "--audio-offset", "0", "--audio-wave", "sine", "--audio-freq",
        "440,660", "--audio-volume", "0.5", "-g", "group-1"};

    void splitting()
    {
        auto error = std::string{};
        auto const w = senders::splitArgs(Chart, error);
        check(w.has_value(), "the chart's command line is taken");
        if (!w)
        {
            return;
        }
        check(w->domain == "/run/mxl/domain-1" && w->groupHint == "group-1", "the domain and the group are read");
        check(w->videoDefinition == "/etc/flow-config/video-flow.json" && w->audioDefinition == "/etc/flow-config/audio-flow.json",
            "both flow definitions are read");
        for (auto const* args : {&w->video, &w->audio})
        {
            check(has(*args, "-d", "/run/mxl/domain-1") && has(*args, "-g", "group-1"),
                "each writer writes into the domain under the group the command line names");
        }
        check(has(w->video, "-v", "/etc/flow-config/video-flow.json") && has(w->video, "-p", "ball") && has(w->video, "-t", "TEXT") &&
                  has(w->video, "--video-offset", "2"),
            "the video writer gets every video option");
        check(!hasFlag(w->video, "-a") && !hasFlag(w->video, "--audio-freq") && !hasFlag(w->video, "--audio-offset"),
            "the video writer writes no audio flow");
        check(has(w->audio, "-a", "/etc/flow-config/audio-flow.json") && has(w->audio, "--audio-freq", "440,660") &&
                  has(w->audio, "--audio-volume", "0.5") && has(w->audio, "--audio-wave", "sine"),
            "the audio writer gets every audio option");
        check(!hasFlag(w->audio, "-v") && !hasFlag(w->audio, "-p") && !hasFlag(w->audio, "-t"), "the audio writer writes no video flow");

        auto const video = senders::splitArgs({"-d", "/d", "-v", "/v.json"}, error);
        check(video && video->audio.empty() && video->groupHint == "mxl-gst-testsrc-group",
            "a video-only line runs one writer, under the test source's own default group");

        auto const refused = [&](std::vector<std::string> const& args, char const* what)
        {
            auto e = std::string{};
            check(!senders::splitArgs(args, e) && !e.empty(), what);
        };
        refused({"-d", "/d", "-v", "/v.json", "--frobnicate", "1"}, "an option the test source does not take is refused");
        refused({"-d", "/d", "-v", "/v.json", "-p"}, "an option without its value is refused");
        refused({"-v", "/v.json"}, "a line without a domain is refused");
        refused({"-d", "/d"}, "a line without a flow is refused");
        refused({"-d", "/d", "-v", "/v.json", "--audio-freq", "440"}, "an audio option without an audio flow is refused");
        refused({"-d", "/d", "-a", "/a.json", "-p", "smpte"}, "a video option without a video flow is refused");
    }

    void metricsPorts()
    {
        auto error = std::string{};
        auto args = Chart;
        args.insert(args.end(), {"--metrics-port", "9103"});
        auto const w = senders::splitArgs(args, error);
        check(w && w->metricsPort == 9103, "the Node takes the metrics port");
        if (!w)
        {
            return;
        }
        // The Node holds the port; a writer bound to it as well would fail,
        // and one bound to every interface would be a second target.
        check(has(w->video, "--metrics-port", "9104") && has(w->video, "--metrics-address", "127.0.0.1") &&
                  has(w->audio, "--metrics-port", "9105") && has(w->audio, "--metrics-address", "127.0.0.1"),
            "each writer serves its metrics on loopback, on a port of its own above the Node's");
        check(!senders::splitArgs(Chart, error)->video.empty() && !hasFlag(senders::splitArgs(Chart, error)->video, "--metrics-port"),
            "without the option no writer serves metrics");

        auto const refused = [&](std::string const& port, char const* what)
        {
            auto e = std::string{};
            check(!senders::splitArgs({"-d", "/d", "-v", "/v.json", "--metrics-port", port}, e) && !e.empty(), what);
        };
        refused("0", "port 0 is refused");
        refused("65534", "a port without two above it for the writers is refused");
        refused("91o3", "a port that is not a number is refused");
        refused("", "an empty port is refused");
        refused("-d", "an option in place of the port is refused");
    }

    void merging()
    {
        auto const video = std::string{
            "# HELP mxl_writer_flow_info Media type.\n"
            "# TYPE mxl_writer_flow_info gauge\n"
            "mxl_writer_flow_info{flow=\"v\",media_type=\"video/v210\"} 1\n"
            "# HELP mxl_writer_latency_seconds Commit latency.\n"
            "# TYPE mxl_writer_latency_seconds histogram\n"
            "mxl_writer_latency_seconds_bucket{flow=\"v\",le=\"+Inf\"} 3\n"
            "mxl_writer_latency_seconds_sum{flow=\"v\"} 0.1\n"
            "mxl_writer_latency_seconds_count{flow=\"v\"} 3\n"};
        auto const audio = std::string{
            "# HELP mxl_writer_flow_info Media type.\n"
            "# TYPE mxl_writer_flow_info gauge\n"
            "mxl_writer_flow_info{flow=\"a\",media_type=\"audio/float32\"} 1\n"
            "# HELP mxl_writer_samples_total Only audio has these.\n"
            "# TYPE mxl_writer_samples_total counter\n"
            "mxl_writer_samples_total{flow=\"a\"} 480\n"
            "# HELP mxl_writer_latency_seconds Commit latency.\n"
            "# TYPE mxl_writer_latency_seconds histogram\n"
            "mxl_writer_latency_seconds_bucket{flow=\"a\",le=\"+Inf\"} 5\n"
            "mxl_writer_latency_seconds_sum{flow=\"a\"} 0.2\n"
            "mxl_writer_latency_seconds_count{flow=\"a\"} 5\n"};
        auto const merged = metrics::merge({video, audio});
        auto const expected = std::string{
            "# HELP mxl_writer_flow_info Media type.\n"
            "# TYPE mxl_writer_flow_info gauge\n"
            "mxl_writer_flow_info{flow=\"v\",media_type=\"video/v210\"} 1\n"
            "mxl_writer_flow_info{flow=\"a\",media_type=\"audio/float32\"} 1\n"
            "# HELP mxl_writer_latency_seconds Commit latency.\n"
            "# TYPE mxl_writer_latency_seconds histogram\n"
            "mxl_writer_latency_seconds_bucket{flow=\"v\",le=\"+Inf\"} 3\n"
            "mxl_writer_latency_seconds_sum{flow=\"v\"} 0.1\n"
            "mxl_writer_latency_seconds_count{flow=\"v\"} 3\n"
            "mxl_writer_latency_seconds_bucket{flow=\"a\",le=\"+Inf\"} 5\n"
            "mxl_writer_latency_seconds_sum{flow=\"a\"} 0.2\n"
            "mxl_writer_latency_seconds_count{flow=\"a\"} 5\n"
            "# HELP mxl_writer_samples_total Only audio has these.\n"
            "# TYPE mxl_writer_samples_total counter\n"
            "mxl_writer_samples_total{flow=\"a\"} 480\n"};
        check(merged == expected, "each family is written once, with the samples of every writer, a histogram's series together");
        if (merged != expected)
        {
            std::fprintf(stderr, "%s", merged.c_str());
        }
        check(metrics::merge({video}) == video, "one writer's metrics pass unchanged");
        check(metrics::merge({}).empty(), "no writer running leaves an empty exposition");
    }

    void fetching()
    {
        auto server = http::Server{};
        server.route = [](std::string const& path)
        {
            return path == "/metrics" ? http::Response{.body = "m 1\n"} : http::Response{.code = 404, .body = "no\n"};
        };
        auto const port = server.listen(0, true);
        check(port.has_value(), "a loopback server for the fetch starts");
        if (!port)
        {
            return;
        }
        auto stop = std::atomic<bool>{false};
        auto thread = std::thread{[&] { server.run(stop); }};
        check(metrics::fetch(*port, std::chrono::milliseconds{1000}) == std::optional<std::string>{"m 1\n"},
            "a writer's metrics are fetched");
        stop = true;
        thread.join();
        check(!metrics::fetch(*port, std::chrono::milliseconds{1000}), "a writer that is not running is left out");

        // A writer that accepts and never answers, as a stopped (SIGSTOP)
        // process does, costs the scrape its timeout and no more.
        auto silent = http::Server{};
        auto const silentPort = silent.listen(0, true);
        check(silentPort.has_value(), "a silent loopback listener starts");
        if (silentPort)
        {
            auto const start = std::chrono::steady_clock::now();
            check(!metrics::fetch(*silentPort, std::chrono::milliseconds{200}), "a writer that does not answer is left out");
            check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds{1000}, "it is given up on at the timeout");
            ::close(silent.listener);
        }
    }

    void uuids()
    {
        check(senders::isUuid("5f5d0000-0000-4000-8000-000000000001"), "a lowercase UUID is one");
        check(!senders::isUuid("5F5D0000-0000-4000-8000-000000000001"), "an uppercase UUID is not what BCP-007-03 carries");
        check(!senders::isUuid("5f5d0000000040008000000000000001"), "a UUID without hyphens is not one");
        check(!senders::isUuid("cam-1"), "a name is not a UUID");
    }

    std::string tempDir()
    {
        char path[] = "/tmp/mxl-writer-nmos-test.XXXXXX";
        auto const* dir = ::mkdtemp(path);
        return dir != nullptr ? dir : "";
    }

    void write(std::string const& path, std::string const& text)
    {
        auto* file = std::fopen(path.c_str(), "w");
        if (file != nullptr)
        {
            std::fputs(text.c_str(), file);
            std::fclose(file);
        }
    }

    void domains()
    {
        auto const root = tempDir();
        auto error = std::string{};
        check(!senders::domainId(root, error) && error.find("domain_def.json") != std::string::npos,
            "a domain without its definition has no id, and says which file is missing");

        write(root + "/domain_def.json", R"({"id": "128cc1c3-9cf8-4946-ad52-7f25a5165fd9", "label": "", "tags": {}})");
        auto const id = senders::domainId(root, error);
        check(id && *id == "128cc1c3-9cf8-4946-ad52-7f25a5165fd9", "the id is the one domain_def.json carries");
        check(senders::domainId(root + "/", error).has_value(), "a trailing slash names the same domain");

        write(root + "/domain_def.json", R"({"id": "128CC1C3-9CF8-4946-AD52-7F25A5165FD9"})");
        check(!senders::domainId(root, error), "an id that is not a lowercase UUID is refused");
        write(root + "/domain_def.json", "not json");
        check(!senders::domainId(root, error), "an unreadable definition is refused");

        auto const named = root + "/fec11c1b-9fab-4275-ab2c-ef676fa2e081";
        std::filesystem::create_directory(named);
        write(named + "/domain_def.json", R"({"id": "128cc1c3-9cf8-4946-ad52-7f25a5165fd9"})");
        check(!senders::domainId(named, error), "a domains/ID directory carrying another id is refused");
        write(named + "/domain_def.json", R"({"id": "fec11c1b-9fab-4275-ab2c-ef676fa2e081"})");
        check(senders::domainId(named, error).has_value(), "a domains/ID directory carrying its own id is taken");
        std::filesystem::remove_all(root);
    }

    // The ConfigMaps the mxl-writer chart renders.
    constexpr char const* Video = R"({"description": "Video output for 5f5d0000-0000-4000-8000-000000000001",
        "id": "5f5d0000-0000-4000-8000-000000000001", "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["group-1:Video"]},
        "format": "urn:x-nmos:format:video", "label": "Video output for 5f5d0000-0000-4000-8000-000000000001", "parents": [],
        "media_type": "video/v210", "grain_rate": {"numerator": 50, "denominator": 1}, "frame_width": 1920, "frame_height": 1080,
        "interlace_mode": "progressive", "colorspace": "BT709", "components": [{"name": "Y", "width": 1920, "height": 1080, "bit_depth": 10},
        {"name": "Cb", "width": 960, "height": 1080, "bit_depth": 10}, {"name": "Cr", "width": 960, "height": 1080, "bit_depth": 10}]})";
    constexpr char const* Audio = R"({"description": "Audio output", "id": "5f5d0000-0000-4000-8000-000000000002",
        "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["group-1:Audio"]}, "format": "urn:x-nmos:format:audio",
        "label": "Audio output", "parents": [], "media_type": "audio/float32", "sample_rate": {"numerator": 48000},
        "channel_count": 2, "bit_depth": 32})";

    std::string tag(picojson::object const& definition, char const* key)
    {
        auto const& tags = definition.at("tags").get<picojson::object>();
        auto const it = tags.find(key);
        if (it == tags.end() || !it->second.is<picojson::array>() || it->second.get<picojson::array>().size() != 1)
        {
            return "(not one value)";
        }
        return it->second.get<picojson::array>()[0].get<std::string>();
    }

    void definitions()
    {
        auto const root = tempDir();
        auto error = std::string{};
        write(root + "/video.json", Video);
        write(root + "/audio.json", Audio);

        auto const video = senders::readFlow("video", root + "/video.json", error);
        check(video && video->flowId == "5f5d0000-0000-4000-8000-000000000001", "a Sender stands for the flow its definition names");
        auto const audio = senders::readFlow("audio", root + "/audio.json", error);
        check(audio.has_value(), "an audio definition is read");

        write(root + "/upper.json", R"({"id": "5F5D0000-0000-4000-8000-000000000001"})");
        check(!senders::readFlow("video", root + "/upper.json", error), "a flow id that is not a lowercase UUID is refused");
        check(!senders::readFlow("video", root + "/missing.json", error), "a missing definition is refused");
        std::filesystem::remove_all(root);
        if (!video || !audio)
        {
            return;
        }

        auto const domain = "128cc1c3-9cf8-4946-ad52-7f25a5165fd9";
        auto const v = senders::parseObject(senders::transportFile(*video, "group-1", "node-1", domain));
        check(v.has_value(), "the transport file is JSON");
        if (v)
        {
            check(senders::textField(*v, "id") == video->flowId, "the flow id stays, so mxl_flow_id is constrained to it");
            check(tag(*v, "urn:x-nmos:tag:grouphint/v1.0") == "group-1:Video",
                "the Sender groups its flow by the group the command line names, under the role controllers expect");
            check(tag(*v, "urn:x-nvnmos:tag:name") == "video", "the Sender is named for its flow");
            check(tag(*v, "urn:x-nvnmos:tag:mxl-domain-id") == domain, "the one domain written into is the Sender's");
            check(senders::textField(*v, "label") == "node-1 video", "the Sender is labelled after the Node and its flow");
            check(senders::textField(*v, "description") == "Video output for 5f5d0000-0000-4000-8000-000000000001",
                "the flow's own description is kept");
            check(senders::textField(*v, "media_type") == "video/v210" && v->at("frame_width").get<double>() == 1920 &&
                      v->at("components").get<picojson::array>().size() == 3,
                "the format is the flow's");
        }
        auto const a = senders::parseObject(senders::transportFile(*audio, "group-1", "node-1", domain));
        check(a && tag(*a, "urn:x-nmos:tag:grouphint/v1.0") == "group-1:Audio", "the audio Sender carries the audio role");
        check(a && a->at("sample_rate").get<picojson::object>().at("denominator").get<double>() == 1,
            "a sample rate without a denominator states 1");
        check(a && a->at("channel_count").get<double>() == 2, "the channel count is the flow's");
    }
}

namespace
{
    void repeating()
    {
        constexpr std::int64_t Second = 1'000'000'000LL;
        auto limiter = repeats::Limiter{60 * Second};
        auto held = std::uint64_t{0};
        check(limiter.admit("registry unreachable", 0, held) && held == 0, "a line is printed the first time");
        auto printed = 0;
        for (auto i = 1; i <= 239; ++i)
        {
            printed += limiter.admit("registry unreachable", i * Second / 4, held) ? 1 : 0;
        }
        check(printed == 0, "the same line four times a second is held back for the minute");
        check(limiter.admit("node up", Second, held) && held == 0, "another line is printed meanwhile");
        check(limiter.admit("registry unreachable", 60 * Second, held) && held == 239,
            "after the minute it is printed again, with how many were held back");
        check(!limiter.admit("registry unreachable", 61 * Second, held), "and held back again for the next minute");

        auto outage = repeats::Limiter{60 * Second};
        outage.admit("unreachable", 0, held);
        for (auto i = 1; i <= 120; ++i)
        {
            outage.admit("unreachable", i * Second / 4, held);
        }
        check(outage.stopped(60 * Second).empty(), "a line seen within the minute has no last count yet");
        auto const last = outage.stopped(90 * Second);
        check(last.size() == 1 && last[0].line == "unreachable" && last[0].heldBack == 120 && last[0].lastSeenNs == 30 * Second,
            "a line quiet for a minute gets its last count, with when it was last seen");
        check(outage.stopped(200 * Second).empty(), "and gets it once");

        auto bounded = repeats::Limiter{60 * Second};
        for (std::size_t i = 0; i < 10 * repeats::Limiter::MaxEntries; ++i)
        {
            bounded.admit("line " + std::to_string(i), 0, held);
        }
        check(bounded.admit("line 0", Second, held), "a table full of lines that never repeat is forgotten, not grown");
    }
}

namespace
{
    std::string podName(std::string const& address, std::string const& ns, std::string const& resolvConf)
    {
        auto in = std::istringstream{resolvConf};
        return host_name::podDnsName(address, ns, in);
    }

    void hostNames()
    {
        // The search list cluster DNS gives a pod, followed by the node's own.
        auto const pod = "search lane.svc.cluster.local svc.cluster.local cluster.local example.org\n"
                         "nameserver 10.96.0.10\n"
                         "options ndots:5\n";
        check(podName("10.244.5.119", "lane", pod) == "10-244-5-119.lane.pod.cluster.local",
            "an IPv4 address becomes the pod's DNS name");
        check(podName("10.0.0.7", "lane", "search lane.svc.k8s.example svc.k8s.example k8s.example\n") ==
                  "10-0-0-7.lane.pod.k8s.example",
            "the cluster domain is read from the search list, not assumed");
        check(podName("fd00::1", "lane", pod) == "fd00--1.lane.pod.cluster.local", "an IPv6 address has its colons dashed");
        check(podName("10.0.0.7", "lane", "search lane.svc.cluster.local.\n") == "10-0-0-7.lane.pod.cluster.local",
            "a trailing dot is dropped");
        // Another namespace's entry, or none, gives no name: a guessed domain
        // would advertise a host no controller resolves.
        check(podName("10.0.0.7", "lane", "search other.svc.cluster.local svc.cluster.local\n").empty(),
            "another namespace's search entry is not taken");
        check(podName("10.0.0.7", "lane", "nameserver 10.96.0.10\n").empty(), "no search list gives no name");
        check(podName("10.0.0.7", "lane", "search lane.svc.\n").empty(), "an empty cluster domain gives no name");
        check(podName("10.0.0.7", "lane", "search lane.svc..\n").empty(), "a lone dot is no cluster domain");
        // The resolver reads the last search line only.
        check(podName("10.0.0.7", "lane", "search lane.svc.old.example\nsearch lane.svc.cluster.local\n") ==
                  "10-0-0-7.lane.pod.cluster.local",
            "the last search line is the one read");
        check(podName("10.0.0.7", "lane", "search lane.svc.cluster.local\nsearch example.org\n").empty(),
            "an entry on an earlier search line is not read");

        // Which host name the Node is given: an explicit one first, then the
        // pod's, and never NvNmos's default or the address.
        auto host = [](std::string const& explicitName, std::string const& ns, std::string const& resolvConf, std::string& error) {
            auto in = std::istringstream{resolvConf};
            error.clear();
            return host_name::nodeHostName(explicitName, "10.244.5.119", ns, in, error);
        };
        auto error = std::string{};
        auto got = host("node.lane.example", "lane", pod, error);
        check(got && *got == "node.lane.example", "an explicit host name takes precedence over the pod's");
        got = host("node.lane.example", "", "", error);
        check(got && *got == "node.lane.example", "an explicit host name needs no namespace or search list");
        got = host("", "lane", pod, error);
        check(got && *got == "10-244-5-119.lane.pod.cluster.local", "without one, a namespace gives the pod's DNS name");
        got = host("", "", pod, error);
        check(!got && error.find("NMOS_HOST_NAME or NMOS_POD_NAMESPACE") != std::string::npos, "neither is refused, naming both");
        got = host("", "lane", "search other.svc.cluster.local\n", error);
        check(!got && error.find("lane.svc.") != std::string::npos, "a namespace the search list does not name is refused, not guessed");
        got = host("10.244.5.119", "lane", pod, error);
        check(!got && error.find("address") != std::string::npos, "an IPv4 address is not a host name");
        got = host("fd00::1", "lane", pod, error);
        check(!got && error.find("address") != std::string::npos, "an IPv6 address is not a host name");
        got = host("writer", "lane", pod, error);
        check(!got && error.find("domain") != std::string::npos, "a name without a domain is refused");
    }
}

int main()
{
    hostNames();
    repeating();
    splitting();
    metricsPorts();
    merging();
    fetching();
    uuids();
    domains();
    definitions();
    if (failures == 0)
    {
        std::fprintf(stderr, "all checks pass\n");
    }
    return failures == 0 ? 0 : 1;
}
