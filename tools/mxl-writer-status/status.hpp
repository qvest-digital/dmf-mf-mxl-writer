// What mxl-writer-status works out about a flow from its definition and the
// header libmxl keeps for it, and how that is rendered for /status and
// /metrics. Kept apart from the libmxl calls and the HTTP server so it can be
// tested without a domain.

#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <deque>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <picojson/wrapper.h>
#include <mxl/dataformat.h>
#include <mxl/flowinfo.h>
#include <mxl/time.h>

namespace status
{
    constexpr std::uint64_t NsPerSecond = 1'000'000'000ULL;

    // What a flow declares about itself, read from its flow definition.
    struct Declared
    {
        std::string mediaType;
        std::string label;
        std::string groupHint;
        int width = 0;
        int height = 0;
        bool interlaced = false;
        std::string rate;
        int sampleRate = 0;
        int channels = 0;
    };

    // A flow definition is written by whoever creates the flow, so a number
    // in it may be anything a double holds; converting one outside int is
    // undefined, so it is clamped first.
    inline int clampToInt(double value)
    {
        if (std::isnan(value))
        {
            return 0;
        }
        auto const clamped = std::clamp(
            value, static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max()));
        return static_cast<int>(clamped);
    }

    inline int jsonInt(picojson::object const& object, char const* key)
    {
        auto const it = object.find(key);
        return (it != object.end() && it->second.is<double>()) ? clampToInt(it->second.get<double>()) : 0;
    }

    // a - b for two indices or times, saturated rather than wrapped.
    inline std::int64_t signedDifference(std::uint64_t a, std::uint64_t b)
    {
        constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (a >= b)
        {
            return static_cast<std::int64_t>(std::min(a - b, max));
        }
        return -static_cast<std::int64_t>(std::min(b - a, max));
    }

    inline std::string jsonText(picojson::object const& object, char const* key)
    {
        auto const it = object.find(key);
        return (it != object.end() && it->second.is<std::string>()) ? it->second.get<std::string>() : std::string{};
    }

    inline std::optional<Declared> parseDefinition(std::string const& text)
    {
        auto json = picojson::value{};
        if (!picojson::parse(json, text).empty() || !json.is<picojson::object>())
        {
            return std::nullopt;
        }
        auto const& object = json.get<picojson::object>();
        auto declared = Declared{};
        declared.mediaType = jsonText(object, "media_type");
        declared.label = jsonText(object, "label");
        declared.interlaced = jsonText(object, "interlace_mode").starts_with("interlaced");
        declared.width = jsonInt(object, "frame_width");
        declared.height = jsonInt(object, "frame_height");
        declared.channels = jsonInt(object, "channel_count");
        if (auto const it = object.find("tags"); it != object.end() && it->second.is<picojson::object>())
        {
            auto const& tags = it->second.get<picojson::object>();
            if (auto const hint = tags.find("urn:x-nmos:tag:grouphint/v1.0");
                hint != tags.end() && hint->second.is<picojson::array>() && !hint->second.get<picojson::array>().empty() &&
                hint->second.get<picojson::array>().front().is<std::string>())
            {
                declared.groupHint = hint->second.get<picojson::array>().front().get<std::string>();
            }
        }
        if (auto const it = object.find("grain_rate"); it != object.end() && it->second.is<picojson::object>())
        {
            auto const& r = it->second.get<picojson::object>();
            auto const den = jsonInt(r, "denominator");
            declared.rate = fmt::format("{}/{}", jsonInt(r, "numerator"), den > 0 ? den : 1);
        }
        if (auto const it = object.find("sample_rate"); it != object.end() && it->second.is<picojson::object>())
        {
            declared.sampleRate = jsonInt(it->second.get<picojson::object>(), "numerator");
        }
        return declared;
    }

    // One grain or one sample, in nanoseconds; 0 for a rate that is not one.
    inline std::uint64_t periodNs(mxlRational const& rate)
    {
        if (rate.numerator <= 0 || rate.denominator <= 0)
        {
            return 0;
        }
        return NsPerSecond * static_cast<std::uint64_t>(rate.denominator) / static_cast<std::uint64_t>(rate.numerator);
    }

    inline bool continuous(mxlFlowConfigInfo const& config)
    {
        return mxlIsContinuousDataFormat(static_cast<int>(config.common.format)) != 0;
    }

    inline char const* unit(mxlFlowConfigInfo const& config)
    {
        if (continuous(config))
        {
            return "samples";
        }
        return mxlIsEventDataFormat(static_cast<int>(config.common.format)) != 0 ? "events" : "grains";
    }

    // Slots in the ring: grains for video and data, samples per channel for
    // audio, entries for an event queue.
    inline std::uint64_t ringLength(mxlFlowConfigInfo const& config)
    {
        if (continuous(config))
        {
            return config.continuous.bufferLength;
        }
        if (mxlIsEventDataFormat(static_cast<int>(config.common.format)) != 0)
        {
            return config.event.eventCount;
        }
        return config.discrete.grainCount;
    }

    // The payload of one ring slot: a whole grain of video, every plane of
    // it, or one float32 sample of every channel of audio. libmxl's slice size
    // is one line of a plane, so a grain is that times the lines it carries:
    // the frame height, or half of it for one field of an interlaced flow. A
    // data grain has no fixed size, so 0 stands for not known.
    inline std::uint64_t slotBytes(mxlFlowConfigInfo const& config, Declared const& declared)
    {
        if (continuous(config))
        {
            return static_cast<std::uint64_t>(config.continuous.channelCount) * sizeof(float);
        }
        if (config.common.format != MXL_DATA_FORMAT_VIDEO || declared.height <= 0)
        {
            return 0;
        }
        auto const lines = static_cast<std::uint64_t>(declared.interlaced ? declared.height / 2 : declared.height);
        auto bytes = std::uint64_t{0};
        for (auto const size : config.discrete.sliceSizes)
        {
            bytes += static_cast<std::uint64_t>(size) * lines;
        }
        return bytes;
    }

    // How long past its media timestamp a commit may land before a reader at
    // the head counts the read as too early. The same rule mxl-reader applies:
    // the span one read covers, for samples ten milliseconds or the writer's
    // sync batch, or the writer's commit batch if larger, plus 50 ms.
    inline std::uint64_t graceNs(mxlFlowConfigInfo const& config)
    {
        auto const& rate = config.common.grainRate;
        auto const period = periodNs(rate);
        if (!continuous(config))
        {
            return period + NsPerSecond / 20;
        }
        auto batch = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(rate.numerator / std::max<std::int64_t>(1, rate.denominator) / 100));
        batch = std::max<std::uint64_t>(batch, config.common.maxSyncBatchSizeHint);
        auto const commit = static_cast<std::uint64_t>(config.common.maxCommitBatchSizeHint) * period;
        return std::max(batch * period, commit) + NsPerSecond / 20;
    }

    // A change of the flow's head seen by the poller, or a stall. Times are
    // TAI nanoseconds: a discrete commit's is the flow's own lastWriteTime; a
    // sample commit's, and a stall's, when the poller saw it, because libmxl's
    // continuous writer moves the head but never stamps lastWriteTime.
    struct Commit
    {
        std::uint64_t seq = 0;
        std::uint64_t head = 0;
        std::uint64_t atNs = 0;
        // Write time minus the media timestamp of the head index: positive is a
        // writer behind the media clock, negative one ahead of it.
        std::int64_t afterMediaNs = 0;
        // What the head moved by. More than one grain is grains never written;
        // for samples it is the size of the commit.
        std::int64_t step = 0;
        // k advanced, g advanced past grains never written, r the same index
        // written again, b stepped backwards, t no commit for a whole timeout.
        char kind = 'k';
    };

    // Follows the head of one flow from successive reads of its runtime info,
    // and turns every change of head index or write time into a Commit.
    struct Tracker
    {
        bool continuous = false;
        mxlRational rate{};
        std::uint64_t timeoutNs = NsPerSecond / 10;

        bool started = false;
        std::uint64_t head = 0;
        std::uint64_t writeNs = 0;
        // When the last commit happened, as far as it is known: the flow's
        // write time for grains, the poll that saw it for samples.
        std::uint64_t committedAt = 0;
        std::uint64_t changedAt = 0;
        bool stalled = false;

        std::uint64_t commits = 0;
        std::uint64_t skipped = 0;
        std::uint64_t backwards = 0;
        std::uint64_t stalls = 0;

        // A head of zero or MXL_UNDEFINED_INDEX means nothing has been
        // committed yet; the first real head is where counting starts.
        std::optional<Commit> observe(std::uint64_t newHead, std::uint64_t newWriteNs, std::uint64_t now)
        {
            if (newHead == 0 || newHead == MXL_UNDEFINED_INDEX)
            {
                return std::nullopt;
            }
            if (!started)
            {
                started = true;
                head = newHead;
                writeNs = newWriteNs;
                committedAt = continuous ? now : newWriteNs;
                changedAt = now;
                return std::nullopt;
            }
            if (newHead == head && (continuous || newWriteNs == writeNs))
            {
                // A clock stepped back is no time passed: the wait starts over
                // rather than reading as a stall.
                if (now < changedAt)
                {
                    changedAt = now;
                    return std::nullopt;
                }
                if (!stalled && now - changedAt >= timeoutNs)
                {
                    stalled = true;
                    ++stalls;
                    return Commit{.head = head, .atNs = now, .kind = 't'};
                }
                return std::nullopt;
            }
            auto commit = Commit{.head = newHead, .atNs = continuous ? now : newWriteNs};
            commit.step = signedDifference(newHead, head);
            commit.afterMediaNs = signedDifference(commit.atNs, mxlIndexToTimestamp(&rate, newHead));
            if (commit.step < 0)
            {
                commit.kind = 'b';
                ++backwards;
            }
            else if (commit.step == 0)
            {
                commit.kind = 'r';
            }
            else if (!continuous && commit.step > 1)
            {
                commit.kind = 'g';
                skipped += static_cast<std::uint64_t>(commit.step - 1);
            }
            ++commits;
            head = newHead;
            writeNs = newWriteNs;
            committedAt = commit.atNs;
            changedAt = now;
            stalled = false;
            return commit;
        }
    };

    // Grains or samples per second the head advanced over the last window of
    // commits. For grains it is on the flow's own clock: head index and write
    // time both come from the flow, so how often or how evenly it is polled,
    // or how late a poll returns, does not enter the figure. For samples the
    // time is the poll's, within one poll interval. The window does not reach back
    // past a backwards step; none with fewer than two commits to measure.
    inline std::optional<double> headRate(std::deque<Commit> const& events, std::uint64_t windowNs)
    {
        auto last = events.rbegin();
        while (last != events.rend() && last->kind == 't')
        {
            ++last;
        }
        if (last == events.rend())
        {
            return std::nullopt;
        }
        auto base = last;
        for (auto it = std::next(last); it != events.rend() && base->kind != 'b' && last->atNs - base->atNs < windowNs; ++it)
        {
            if (it->kind != 't')
            {
                base = it;
            }
        }
        if (base == last || last->atNs <= base->atNs)
        {
            return std::nullopt;
        }
        return static_cast<double>(last->head - base->head) * 1e9 / static_cast<double>(last->atNs - base->atNs);
    }

    // A flow's data file, by device and inode. libmxl creates a flow anew
    // under a new data file, which a reader still holding the old one never
    // sees: its head stays where the old file left it.
    struct FileIdentity
    {
        std::uint64_t device = 0;
        std::uint64_t inode = 0;
        bool operator==(FileIdentity const&) const = default;
    };

    // Whether the flow open on `opened` is no longer the one in the domain:
    // its data file is gone, or another is there. `opened` is read before the
    // reader opens, so a flow replaced in between counts as replaced, and
    // reopening costs one second; read after, the reader could hold the old
    // file while the identity names the new one, and it would never reopen.
    inline bool replaced(std::optional<FileIdentity> const& opened, std::optional<FileIdentity> const& current)
    {
        return !opened || !current || *opened != *current;
    }

    // Everything /status and /metrics say about one flow, copied out of the
    // live state so rendering needs no lock and no clock of its own.
    struct FlowView
    {
        std::string id;
        std::string state;
        bool open = false;

        std::string unit;
        std::string mediaType;
        std::string label;
        std::string groupHint;
        std::string format;
        mxlRational rate{};
        std::uint64_t ringLength = 0;
        std::uint64_t slotBytes = 0;
        std::uint64_t periodNs = 0;
        std::uint32_t commitBatchHint = 0;
        std::uint32_t syncBatchHint = 0;
        bool deviceMemory = false;

        // From the flow's header; zero until the writer has committed. writeNs
        // is the last commit's time as the Tracker knows it.
        std::uint64_t head = 0;
        std::uint64_t writeNs = 0;
        std::uint64_t readNs = 0;
        std::optional<double> headRate;

        std::uint64_t commits = 0;
        std::uint64_t skipped = 0;
        std::uint64_t backwards = 0;
        std::uint64_t stalls = 0;
        std::uint64_t opens = 0;
        std::uint64_t lost = 0;

        std::string lastError;
        std::uint64_t lastErrorAt = 0;
    };

    inline bool committed(FlowView const& flow)
    {
        return flow.open && flow.head != 0 && flow.head != MXL_UNDEFINED_INDEX;
    }

    // Writing means a commit landed within the last second.
    inline bool writing(FlowView const& flow, std::uint64_t now)
    {
        return committed(flow) && flow.writeNs != 0 && now >= flow.writeNs && now - flow.writeNs < NsPerSecond;
    }

    // The head index minus the index the media clock stands at now: positive
    // is a writer ahead of the clock, negative one behind it.
    inline std::int64_t headVsNow(FlowView const& flow, std::uint64_t now)
    {
        return signedDifference(flow.head, mxlTimestampToIndex(&flow.rate, now));
    }

    // libmxl records reads of grains and events in lastReadTime; a sample
    // read leaves it at the flow's creation, so it says nothing for audio.
    inline bool readTimeKept(FlowView const& flow)
    {
        return committed(flow) && flow.unit != "samples" && flow.readNs != 0;
    }

    inline double nominalRate(FlowView const& flow)
    {
        return flow.rate.denominator > 0 ? static_cast<double>(flow.rate.numerator) / static_cast<double>(flow.rate.denominator) : 0.0;
    }

    inline double secondsBetween(std::uint64_t later, std::uint64_t earlier)
    {
        return (static_cast<double>(later) - static_cast<double>(earlier)) / 1e9;
    }

    // A JSON string literal with '/' written as itself. picojson writes it as
    // "\/", which is valid JSON, but a client that matches the body's
    // strings as text does not find "video/v210" there.
    inline std::string jsonString(std::string const& value)
    {
        auto const escaped = picojson::value{value}.serialize();
        auto out = std::string{};
        out.reserve(escaped.size());
        for (auto i = std::size_t{0}; i < escaped.size(); ++i)
        {
            // The character after a backslash belongs to its escape and is
            // copied as is; only "\/" also drops the backslash.
            if ((escaped[i] == '\\') && (i + 1 < escaped.size()))
            {
                if (escaped[i + 1] != '/')
                {
                    out += escaped[i];
                }
                out += escaped[++i];
                continue;
            }
            out += escaped[i];
        }
        return out;
    }

    inline std::string jsonNumber(std::optional<double> value, int decimals)
    {
        return value ? fmt::format("{:.{}f}", *value, decimals) : std::string{"null"};
    }

    // Prometheus label values come from flow definitions, which anybody
    // writing a flow controls.
    inline std::string labelValue(std::string const& value)
    {
        auto out = std::string{};
        for (auto const c : value)
        {
            auto const ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' ||
                            c == '+' || c == '-' || c == ':';
            out += ok ? c : '_';
        }
        return out;
    }

    inline std::string statusJson(std::string const& version, std::vector<FlowView> const& flows, std::uint64_t now)
    {
        auto out = fmt::format("{{\"version\":{},\"flows\":[", jsonString(version));
        for (std::size_t i = 0; i < flows.size(); ++i)
        {
            auto const& f = flows[i];
            out += i ? "," : "";
            out += fmt::format("{{\"id\":{},\"state\":{},\"writing\":{}", jsonString(f.id), jsonString(f.state), writing(f, now) ? "true" : "false");
            if (f.open)
            {
                auto const ringSeconds = static_cast<double>(f.ringLength) * static_cast<double>(f.periodNs) / 1e9;
                out += fmt::format(",\"unit\":{},\"mediaType\":{},\"label\":{},\"groupHint\":{},\"format\":{},\"rate\":\"{}/{}\","
                                   "\"ring\":{{\"length\":{},\"slotBytes\":{},\"bytes\":{},\"seconds\":{:.3f}}},"
                                   "\"commitBatchHint\":{},\"syncBatchHint\":{},\"payloadLocation\":\"{}\"",
                    jsonString(f.unit),
                    jsonString(f.mediaType),
                    jsonString(f.label),
                    jsonString(f.groupHint),
                    jsonString(f.format),
                    f.rate.numerator,
                    f.rate.denominator,
                    f.ringLength,
                    f.slotBytes ? fmt::format("{}", f.slotBytes) : std::string{"null"},
                    f.slotBytes ? fmt::format("{}", f.slotBytes * f.ringLength) : std::string{"null"},
                    ringSeconds,
                    f.commitBatchHint,
                    f.syncBatchHint,
                    f.deviceMemory ? "device" : "host");
            }
            if (committed(f))
            {
                auto const lead = headVsNow(f, now);
                out += fmt::format(",\"head\":{},\"headVsNow\":{},\"headVsNowMs\":{:.3f},\"headRate\":{},\"nominalRate\":{:.3f},"
                                   "\"writeAgeSeconds\":{:.6f}",
                    f.head,
                    lead,
                    static_cast<double>(lead) * static_cast<double>(f.periodNs) / 1e6,
                    jsonNumber(f.headRate, 3),
                    nominalRate(f),
                    secondsBetween(now, f.writeNs));
                if (readTimeKept(f))
                {
                    out += fmt::format(",\"readAgeSeconds\":{:.6f}", secondsBetween(now, f.readNs));
                }
            }
            out += fmt::format(",\"commits\":{},\"skipped\":{},\"backwardSteps\":{},\"stalls\":{},\"opens\":{},\"lost\":{}",
                f.commits,
                f.skipped,
                f.backwards,
                f.stalls,
                f.opens,
                f.lost);
            out += f.lastErrorAt
                     ? fmt::format(
                           ",\"lastError\":{{\"message\":{},\"secondsAgo\":{:.1f}}}}}", jsonString(f.lastError), secondsBetween(now, f.lastErrorAt))
                     : std::string{",\"lastError\":null}"};
        }
        return out + "]}";
    }

    inline std::string metricsText(std::vector<FlowView> const& flows, std::uint64_t now)
    {
        auto out = std::string{};
        auto const family = [&](char const* name, char const* type, char const* help)
        {
            out += fmt::format("# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
        };
        family("mxl_writer_status_flow_info", "gauge", "Media type and unit of a flow, once it has been opened.");
        for (auto const& f : flows)
        {
            if (f.open)
            {
                out += fmt::format(
                    "mxl_writer_status_flow_info{{flow=\"{}\",media_type=\"{}\",unit=\"{}\"}} 1\n", f.id, labelValue(f.mediaType), f.unit);
            }
        }
        family("mxl_writer_status_flow_state", "gauge", "1 for the state the flow is in.");
        for (auto const& f : flows)
        {
            out += fmt::format("mxl_writer_status_flow_state{{flow=\"{}\",state=\"{}\"}} 1\n", f.id, f.state);
        }
        family("mxl_writer_status_flow_writing", "gauge", "1 while a commit landed within the last second.");
        for (auto const& f : flows)
        {
            out += fmt::format("mxl_writer_status_flow_writing{{flow=\"{}\"}} {}\n", f.id, writing(f, now) ? 1 : 0);
        }

        auto const gauge = [&](char const* name, char const* help, auto const& value, bool (*has)(FlowView const&))
        {
            family(name, "gauge", help);
            for (auto const& f : flows)
            {
                if (has(f))
                {
                    out += fmt::format("{}{{flow=\"{}\"}} {}\n", name, f.id, value(f));
                }
            }
        };
        auto const isOpen = [](FlowView const& f)
        {
            return f.open;
        };
        auto const hasHead = [](FlowView const& f)
        {
            return committed(f);
        };
        gauge("mxl_writer_status_ring_length", "Slots in the ring, in the flow's unit.", [](FlowView const& f) { return f.ringLength; }, isOpen);
        gauge(
            "mxl_writer_status_ring_seconds",
            "Media time the ring holds.",
            [](FlowView const& f) { return fmt::format("{:.6f}", static_cast<double>(f.ringLength) * static_cast<double>(f.periodNs) / 1e9); },
            isOpen);
        gauge(
            "mxl_writer_status_ring_bytes",
            "Payload bytes of the whole ring, where a slot has a fixed size.",
            [](FlowView const& f) { return f.slotBytes * f.ringLength; },
            [](FlowView const& f) { return f.open && f.slotBytes != 0; });
        gauge(
            "mxl_writer_status_nominal_rate",
            "Grains or samples per second the flow declares.",
            [](FlowView const& f) { return fmt::format("{:.6f}", nominalRate(f)); },
            isOpen);
        gauge("mxl_writer_status_head_index", "The flow's head index.", [](FlowView const& f) { return f.head; }, hasHead);
        gauge(
            "mxl_writer_status_head_rate",
            "Grains or samples per second the head advanced over the last second, on the flow's own clock.",
            [](FlowView const& f) { return fmt::format("{:.6f}", *f.headRate); },
            [](FlowView const& f) { return committed(f) && f.headRate.has_value(); });
        gauge(
            "mxl_writer_status_head_lead_seconds",
            "Head index minus the current media-clock index; negative is behind the clock.",
            [now](FlowView const& f)
            { return fmt::format("{:.6f}", static_cast<double>(headVsNow(f, now)) * static_cast<double>(f.periodNs) / 1e9); },
            hasHead);
        gauge(
            "mxl_writer_status_write_age_seconds",
            "Wall clock minus the flow's last write time.",
            [now](FlowView const& f) { return fmt::format("{:.6f}", secondsBetween(now, f.writeNs)); },
            hasHead);
        gauge(
            "mxl_writer_status_read_age_seconds",
            "Wall clock minus the flow's last read time as libmxl records it; grains and events only.",
            [now](FlowView const& f) { return fmt::format("{:.6f}", secondsBetween(now, f.readNs)); },
            readTimeKept);

        auto const counter = [&](char const* name, char const* help, std::uint64_t FlowView::* field)
        {
            family(name, "counter", help);
            for (auto const& f : flows)
            {
                out += fmt::format("{}{{flow=\"{}\"}} {}\n", name, f.id, f.*field);
            }
        };
        counter("mxl_writer_status_commits_total", "Changes of the head index or write time seen.", &FlowView::commits);
        counter("mxl_writer_status_skipped_total", "Grains the head moved past without writing them.", &FlowView::skipped);
        counter("mxl_writer_status_backward_steps_total", "Times the head index moved backwards.", &FlowView::backwards);
        counter("mxl_writer_status_stalls_total", "Timeouts that passed without a commit, counted once until commits resume.", &FlowView::stalls);
        counter("mxl_writer_status_opens_total", "Times the flow was opened.", &FlowView::opens);
        counter("mxl_writer_status_lost_total", "Times an open flow was removed or replaced and had to be opened again.", &FlowView::lost);
        return out;
    }
}
