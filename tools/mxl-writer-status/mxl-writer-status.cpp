// mxl-writer-status: serves the ring-buffer state of the flows a writer
// produces, read from the header libmxl keeps for each flow.
//
// Each flow is opened with a flow reader, but no grain or sample is ever read:
// only the flow's config and runtime info, which libmxl copies out of the
// flow's header. The reader maps the data file read-only and holds the access
// file open without touching it, so lastReadTime still says when a real
// consumer last read; on a read-only volume the access file does not open,
// which libmxl allows. Every change of the head index or write time is kept,
// timed by the write time the flow records where libmxl records one, and
// served as a page on /, server-sent events on /stream, JSON on /status and
// Prometheus text on /metrics.

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/mxl.h>
#include <mxl/time.h>
#include "page.hpp"
#include "server.hpp"
#include "status.hpp"

namespace
{
    using status::NsPerSecond;

    std::atomic<bool> stopping{false};

    void onSignal(int)
    {
        stopping = true;
    }

#ifndef MXL_WRITER_STATUS_VERSION
#   define MXL_WRITER_STATUS_VERSION "dev"
#endif

    // This build and the libmxl it runs against: the second decides the
    // header layout read, and the image's own tag does not name it.
    std::string versionText()
    {
        auto lib = mxlVersionType{};
        mxlGetVersion(&lib);
        return fmt::format("mxl-writer-status {} (libmxl {})", MXL_WRITER_STATUS_VERSION, lib.full != nullptr ? lib.full : "unknown");
    }

    // How often the header is read. Short enough that one poll sees one
    // commit of a video flow up to 240/1; the times come from the flow, so
    // the interval only decides how many commits one poll may merge.
    constexpr std::uint64_t PollNs = 2'000'000ULL;

    // The page's window is six seconds; this keeps that at 600 commits a
    // second and much longer for video.
    constexpr std::size_t EventsKept = 4096;

    struct FlowStats
    {
        std::string id;

        mutable std::mutex mutex;
        status::FlowView view;
        std::deque<status::Commit> events;
        std::uint64_t eventSeq = 0;
        std::uint64_t graceNs = 0;

        void set(char const* state)
        {
            auto const lock = std::lock_guard{mutex};
            view.state = state;
            if (std::string_view{state} != "open")
            {
                view.open = false;
            }
        }

        void error(std::string message)
        {
            auto const lock = std::lock_guard{mutex};
            if (message != view.lastError)
            {
                std::fprintf(stderr, "flow %s: %s\n", id.c_str(), message.c_str());
            }
            view.lastError = std::move(message);
            view.lastErrorAt = mxlGetTime();
        }
    };

    std::optional<status::Declared> readDefinition(mxlInstance instance, std::string const& id)
    {
        auto buffer = std::vector<char>(8192);
        auto size = buffer.size();
        auto result = mxlGetFlowDef(instance, id.c_str(), buffer.data(), &size);
        if (result == MXL_ERR_INVALID_ARG && size > buffer.size())
        {
            buffer.resize(size);
            result = mxlGetFlowDef(instance, id.c_str(), buffer.data(), &size);
        }
        if (result != MXL_STATUS_OK)
        {
            return std::nullopt;
        }
        return status::parseDefinition(std::string{buffer.data()});
    }

    std::optional<status::FileIdentity> dataFile(std::string const& domain, std::string const& id)
    {
        struct stat st{};
        if (::stat(fmt::format("{}/{}.mxl-flow/data", domain, id).c_str(), &st) != 0)
        {
            return std::nullopt;
        }
        return status::FileIdentity{.device = static_cast<std::uint64_t>(st.st_dev), .inode = static_cast<std::uint64_t>(st.st_ino)};
    }

    // libmxl reports a reader it could not open for want of permission as an
    // unknown error, so the files themselves are asked.
    bool permissionDenied(std::string const& domain, std::string const& id)
    {
        auto const dir = fmt::format("{}/{}.mxl-flow", domain, id);
        auto const denied = [](std::string const& path, int mode)
        {
            struct stat st{};
            return ::stat(path.c_str(), &st) == 0 && ::access(path.c_str(), mode) != 0 && (errno == EACCES || errno == EPERM);
        };
        return denied(dir, R_OK | X_OK) || denied(dir + "/flow_def.json", R_OK) || denied(dir + "/data", R_OK);
    }

    // The header is written field by field while it is read, so a copy may
    // pair a new head index with the previous write time. Two equal copies in
    // a row are one the writer did not touch in between.
    mxlStatus runtimeInfo(mxlFlowReader reader, mxlFlowRuntimeInfo& out)
    {
        auto previous = mxlFlowRuntimeInfo{};
        auto result = mxlFlowReaderGetRuntimeInfo(reader, &previous);
        for (auto attempt = 0; result == MXL_STATUS_OK && attempt < 4; ++attempt)
        {
            result = mxlFlowReaderGetRuntimeInfo(reader, &out);
            if (out.headIndex == previous.headIndex && out.lastWriteTime == previous.lastWriteTime)
            {
                return result;
            }
            previous = out;
        }
        return result;
    }

    // Holds the flow open and reads its header every PollNs until the flow is
    // removed or replaced, then opens it again.
    void watchOpen(std::string const& domain, FlowStats& stats, mxlFlowReader reader, mxlFlowInfo const& info, status::Declared const& declared,
        std::optional<status::FileIdentity> const& file)
    {
        auto const& config = info.config;
        auto const period = status::periodNs(config.common.grainRate);
        auto tracker = status::Tracker{.continuous = status::continuous(config), .rate = config.common.grainRate};
        // Several grains, or several commit batches of samples, so a commit
        // that lands late is late rather than a stall.
        tracker.timeoutNs = std::max<std::uint64_t>(
            4 * period * std::max<std::uint32_t>(1, tracker.continuous ? config.common.maxCommitBatchSizeHint : 1), NsPerSecond / 10);
        // Counters run on across reopening the flow, as Prometheus counters do.
        auto base = status::FlowView{};
        {
            auto const lock = std::lock_guard{stats.mutex};
            auto& v = stats.view;
            v.open = true;
            v.state = "open";
            v.unit = status::unit(config);
            v.mediaType = declared.mediaType;
            v.label = declared.label;
            v.groupHint = declared.groupHint;
            v.format = declared.width ? fmt::format("{}x{}{} @ {}", declared.width, declared.height, declared.interlaced ? "i" : "", declared.rate)
                     : declared.sampleRate ? fmt::format("{} ch @ {}", declared.channels, declared.sampleRate)
                                           : declared.rate;
            v.rate = config.common.grainRate;
            v.ringLength = status::ringLength(config);
            v.slotBytes = status::slotBytes(config, declared);
            v.periodNs = period;
            v.commitBatchHint = config.common.maxCommitBatchSizeHint;
            v.syncBatchHint = config.common.maxSyncBatchSizeHint;
            v.deviceMemory = config.common.payloadLocation == MXL_PAYLOAD_LOCATION_DEVICE_MEMORY;
            v.head = 0;
            v.headRate.reset();
            ++v.opens;
            stats.graceNs = status::graceNs(config);
            stats.events.clear();
            base = v;
        }
        std::fprintf(stderr,
            "flow %s: open, %s, %llu %s\n",
            stats.id.c_str(),
            declared.mediaType.c_str(),
            static_cast<unsigned long long>(status::ringLength(config)),
            status::unit(config));

        auto checkedAt = std::uint64_t{0};
        while (!stopping)
        {
            auto runtime = mxlFlowRuntimeInfo{};
            if (auto const result = runtimeInfo(reader, runtime); result != MXL_STATUS_OK)
            {
                stats.set("flow_error");
                stats.error(fmt::format("cannot read the flow's runtime info: status {}", static_cast<int>(result)));
                return;
            }
            auto const now = mxlGetTime();
            auto const commit = tracker.observe(runtime.headIndex, runtime.lastWriteTime, now);
            {
                auto const lock = std::lock_guard{stats.mutex};
                auto& v = stats.view;
                v.head = runtime.headIndex;
                v.writeNs = tracker.committedAt;
                v.readNs = runtime.lastReadTime;
                v.commits = base.commits + tracker.commits;
                v.skipped = base.skipped + tracker.skipped;
                v.backwards = base.backwards + tracker.backwards;
                v.stalls = base.stalls + tracker.stalls;
                if (commit)
                {
                    auto event = *commit;
                    event.seq = ++stats.eventSeq;
                    stats.events.push_back(event);
                    if (stats.events.size() > EventsKept)
                    {
                        stats.events.pop_front();
                    }
                    v.headRate = status::headRate(stats.events, NsPerSecond);
                }
            }
            // A flow removed or created anew under the open reader leaves its
            // head where the old file stopped, or where it was before the
            // first commit; once a second the file says which.
            if (now < checkedAt || now - checkedAt >= NsPerSecond)
            {
                checkedAt = now;
                if (status::replaced(file, dataFile(domain, stats.id)))
                {
                    auto const lock = std::lock_guard{stats.mutex};
                    ++stats.view.lost;
                    return;
                }
            }
            mxlSleepForNs(PollNs);
        }
    }

    void watchFlow(std::string const& domain, FlowStats& stats)
    {
        mxlInstance instance = nullptr;
        while (!stopping)
        {
            if (instance == nullptr)
            {
                instance = mxlCreateInstance(domain.c_str(), "");
                if (instance == nullptr)
                {
                    // The volume may not be mounted yet, or nothing has
                    // created the domain: wait for it rather than exit.
                    stats.set("domain_unavailable");
                    struct stat st{};
                    stats.error(::stat(domain.c_str(), &st) == 0 ? fmt::format("cannot open MXL domain {}", domain)
                                                                 : fmt::format("MXL domain {} does not exist", domain));
                    mxlSleepForNs(NsPerSecond);
                    continue;
                }
            }

            // Before the reader opens, so a replacement in between is seen.
            auto const file = dataFile(domain, stats.id);
            mxlFlowReader reader = nullptr;
            auto result = mxlCreateFlowReader(instance, stats.id.c_str(), "", &reader);
            if (result != MXL_STATUS_OK)
            {
                // The writer has not created it yet, or has gone and taken it
                // with it: keep asking.
                if (result == MXL_ERR_FLOW_NOT_FOUND || result == MXL_ERR_NOT_FOUND)
                {
                    stats.set("flow_not_found");
                    stats.error("flow not found in the domain");
                }
                else if (result == MXL_ERR_PERMISSION_DENIED || permissionDenied(domain, stats.id))
                {
                    stats.set("permission_denied");
                    stats.error("permission denied opening the flow");
                }
                else
                {
                    stats.set("flow_error");
                    stats.error(fmt::format("cannot open the flow: status {}", static_cast<int>(result)));
                }
                mxlSleepForNs(NsPerSecond);
                continue;
            }

            auto info = mxlFlowInfo{};
            result = mxlFlowReaderGetInfo(reader, &info);
            auto const declared = result == MXL_STATUS_OK ? readDefinition(instance, stats.id) : std::nullopt;
            if (declared)
            {
                watchOpen(domain, stats, reader, info, *declared, file);
            }
            else
            {
                stats.set("flow_error");
                stats.error(result == MXL_STATUS_OK ? std::string{"cannot read the flow definition"}
                                                    : fmt::format("cannot read the flow info: status {}", static_cast<int>(result)));
            }
            mxlReleaseFlowReader(instance, reader);
            if (!stopping)
            {
                mxlSleepForNs(declared ? NsPerSecond / 10 : NsPerSecond);
            }
        }
        if (instance != nullptr)
        {
            mxlDestroyInstance(instance);
        }
    }

    std::vector<status::FlowView> views(std::vector<std::unique_ptr<FlowStats>> const& flows)
    {
        auto out = std::vector<status::FlowView>{};
        for (auto const& flow : flows)
        {
            auto const lock = std::lock_guard{flow->mutex};
            out.push_back(flow->view);
        }
        return out;
    }

    double ms(std::int64_t ns)
    {
        return static_cast<double>(ns) / 1e6;
    }

    // One batch for the page: per flow, what its ring is drawn against and the
    // commits since the cursor, each [head, at ms, after media ms, step, kind].
    std::string streamBatch(std::vector<std::unique_ptr<FlowStats>> const& flows, std::vector<std::uint64_t>& cursors)
    {
        auto const now = mxlGetTime();
        if (cursors.size() != flows.size())
        {
            // A new viewer gets the last six seconds first, so the page has
            // something to draw.
            cursors.assign(flows.size(), 0);
            auto const since = now - std::min(now, 6 * NsPerSecond);
            for (std::size_t i = 0; i < flows.size(); ++i)
            {
                auto const lock = std::lock_guard{flows[i]->mutex};
                for (auto const& e : flows[i]->events)
                {
                    if (e.atNs < since)
                    {
                        cursors[i] = e.seq;
                    }
                }
            }
        }
        auto out = fmt::format("{{\"now\":{:.3f},\"flows\":[", ms(static_cast<std::int64_t>(now)));
        for (std::size_t i = 0; i < flows.size(); ++i)
        {
            auto const& flow = *flows[i];
            auto const lock = std::lock_guard{flow.mutex};
            auto const& v = flow.view;
            // What one commit is expected to cover: a grain, or the writer's
            // commit batch of samples.
            auto const spanNs = v.periodNs * (v.unit == "samples" ? std::max<std::uint32_t>(1, v.commitBatchHint) : 1);
            out += fmt::format("{}{{\"id\":{},\"label\":{},\"format\":{},\"unit\":{},\"state\":{},\"ring\":{},\"slotBytes\":{},"
                               "\"periodMs\":{:.6f},\"spanMs\":{:.3f},\"graceMs\":{:.3f},\"rate\":{:.3f},\"head\":{},\"clock\":{},\"ev\":[",
                i ? "," : "",
                status::jsonString(flow.id),
                status::jsonString(v.label),
                status::jsonString(v.format),
                status::jsonString(v.unit),
                status::jsonString(v.state),
                v.open ? v.ringLength : 0,
                v.slotBytes,
                ms(static_cast<std::int64_t>(v.periodNs)),
                ms(static_cast<std::int64_t>(spanNs)),
                ms(static_cast<std::int64_t>(flow.graceNs)),
                status::nominalRate(v),
                status::committed(v) ? v.head : 0,
                v.open && v.periodNs ? mxlTimestampToIndex(&v.rate, now) : 0);
            auto first = true;
            for (auto const& e : flow.events)
            {
                if (e.seq <= cursors[i])
                {
                    continue;
                }
                out += fmt::format("{}[{},{:.3f},{:.3f},{},\"{}\"]",
                    first ? "" : ",",
                    e.head,
                    ms(static_cast<std::int64_t>(e.atNs)),
                    ms(e.afterMediaNs),
                    e.step,
                    e.kind);
                first = false;
            }
            cursors[i] = flow.eventSeq;
            out += "]}";
        }
        return out + "]}";
    }

    http::Response route(std::string const& path, std::vector<std::unique_ptr<FlowStats>> const& flows)
    {
        if (path == "/")
        {
            return {.type = "text/html; charset=utf-8", .body = RingPage};
        }
        if (path == "/stream")
        {
            return {.stream = true};
        }
        if (path == "/metrics")
        {
            return {.type = "text/plain; version=0.0.4", .body = status::metricsText(views(flows), mxlGetTime())};
        }
        if (path == "/status")
        {
            return {.type = "application/json", .body = status::statusJson(versionText(), views(flows), mxlGetTime())};
        }
        if (path == "/healthz")
        {
            return {.body = "ok\n"};
        }
        return {.code = 404, .body = "not found\n"};
    }
}

int main(int argc, char** argv)
{
    auto app = CLI::App{"mxl-writer-status"};
    app.set_version_flag("--version", versionText());
    auto domain = std::string{};
    app.add_option("-d,--domain", domain, "The MXL domain directory.")->required();
    auto flowIds = std::vector<std::string>{};
    app.add_option("-f,--flow", flowIds, "A flow id to watch. Repeat for several.")->required();
    auto port = 9102;
    app.add_option("-p,--port", port, "Port of the page, status and metrics endpoints.")->check(CLI::Range(1, 65535));
    CLI11_PARSE(app, argc, argv);

    // libmxl answers a malformed id as an invalid argument, which the watch
    // loop would take for a flow not there yet and wait on for ever.
    auto const uuid = std::regex{"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"};
    for (auto const& id : flowIds)
    {
        if (!std::regex_match(id, uuid))
        {
            std::fprintf(stderr, "not a lowercase flow UUID: %s\n", id.c_str());
            return 2;
        }
    }

    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    std::fprintf(stderr,
        "%s, watching %zu flow%s in %s, serving on :%d\n",
        versionText().c_str(),
        flowIds.size(),
        flowIds.size() == 1 ? "" : "s",
        domain.c_str(),
        port);

    auto flows = std::vector<std::unique_ptr<FlowStats>>{};
    for (auto const& id : flowIds)
    {
        flows.push_back(std::make_unique<FlowStats>());
        flows.back()->id = id;
        flows.back()->view.id = id;
        flows.back()->view.state = "starting";
    }

    auto server = http::Server{};
    server.route = [&flows](std::string const& path)
    {
        return route(path, flows);
    };
    server.tick = [&flows](std::vector<std::uint64_t>& cursors)
    {
        return streamBatch(flows, cursors);
    };
    if (!server.listen(static_cast<std::uint16_t>(port)))
    {
        std::fprintf(stderr, "cannot listen on :%d\n", port);
        return 1;
    }

    // Every thread is joined before flows goes out of scope.
    auto threads = std::vector<std::thread>{};
    for (auto& flow : flows)
    {
        threads.emplace_back(watchFlow, domain, std::ref(*flow));
    }
    server.run(stopping);
    stopping = true;
    for (auto& thread : threads)
    {
        thread.join();
    }
    return 0;
}
