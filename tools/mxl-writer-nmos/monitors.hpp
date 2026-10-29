// What a Sender's BCP-008 monitor reports, worked out from the metrics its
// writer process serves. Links neither libmxl nor NvNmos, so senders-test.cpp
// covers it on its own.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace monitors
{
    // BCP-008's values. Link: all up, some down, all down. Transmission and
    // essence: inactive, healthy, partially healthy, unhealthy.
    // Synchronization: not used to unhealthy.
    enum : int
    {
        Inactive = 0,
        Healthy = 1,
        PartiallyHealthy = 2,
        Unhealthy = 3,
        AllUp = 1,
        AllDown = 3,
        NotUsed = 0
    };

    // One flow's state as its writer reports it.
    struct Sample
    {
        bool delivering = false;
        std::string state;
        std::map<std::string, std::uint64_t> writes;
    };

    namespace details
    {
        // The value of label name in a sample line's label set, or nothing.
        inline std::optional<std::string_view> label(std::string_view labels, std::string_view name)
        {
            auto const key = std::string{name} + "=\"";
            auto const at = labels.find(key);
            if (at == std::string_view::npos || (at != 0 && labels[at - 1] != ',' && labels[at - 1] != '{'))
            {
                return std::nullopt;
            }
            auto const start = at + key.size();
            auto const end = labels.find('"', start);
            return end == std::string_view::npos ? std::nullopt : std::optional{labels.substr(start, end - start)};
        }
    }

    // The sample of flowId in a writer's Prometheus text, or nothing where the
    // text names no state for it.
    inline std::optional<Sample> parse(std::string_view text, std::string const& flowId)
    {
        auto sample = Sample{};
        auto stated = false;
        while (!text.empty())
        {
            auto const end = text.find('\n');
            auto const line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            auto const open = line.find('{');
            auto const close = line.find('}');
            if (line.starts_with('#') || open == std::string_view::npos || close == std::string_view::npos || close < open)
            {
                continue;
            }
            auto const name = line.substr(0, open);
            auto const labels = line.substr(open, close - open + 1);
            if (details::label(labels, "flow") != std::optional<std::string_view>{flowId})
            {
                continue;
            }
            auto const value = std::string{line.substr(close + 1)};
            auto const number = std::strtod(value.c_str(), nullptr);
            if (name == "mxl_writer_flow_delivering")
            {
                sample.delivering = number == 1.0;
            }
            else if (name == "mxl_writer_flow_state" && number == 1.0)
            {
                if (auto const state = details::label(labels, "state"))
                {
                    sample.state = std::string{*state};
                    stated = true;
                }
            }
            else if (name == "mxl_writer_writes_total")
            {
                if (auto const result = details::label(labels, "result"))
                {
                    sample.writes[std::string{*result}] = static_cast<std::uint64_t>(number);
                }
            }
        }
        return stated ? std::optional{sample} : std::nullopt;
    }

    struct Status
    {
        int value;
        std::string message;
    };

    // What one poll reports per status domain; nothing leaves the status as
    // it was.
    struct Report
    {
        std::optional<Status> link;
        std::optional<Status> transmission;
        Status synchronization{NotUsed, ""};
        std::optional<Status> essence;
    };

    // The statuses of an active Sender from its writer's sample now, or none
    // where the writer does not answer, and the sample a poll before. Writes
    // counted since then decide the essence: input written as invalid is
    // unhealthy, input that arrived too late only partially healthy. The MXL
    // domain is the Sender's only link.
    inline Report report(std::optional<Sample> const& previous, std::optional<Sample> const& current)
    {
        auto out = Report{};
        if (!current)
        {
            out.transmission = Status{Unhealthy, "the writer process does not answer"};
            return out;
        }
        out.link = current->state == "domain_unavailable" ? Status{AllDown, "the MXL domain is unavailable"} : Status{AllUp, ""};
        out.transmission = current->delivering ? Status{Healthy, ""}
                                               : Status{Unhealthy, "no commit within the last second, flow " + current->state};
        auto since = [&](char const* result)
        {
            auto const now = current->writes.contains(result) ? current->writes.at(result) : 0;
            auto const before = previous && previous->writes.contains(result) ? previous->writes.at(result) : 0;
            // A writer started again counts from zero.
            return now >= before ? now - before : now;
        };
        if (auto const invalid = since("invalid_input"); invalid > 0)
        {
            out.essence = Status{Unhealthy, std::to_string(invalid) + " written as invalid input"};
        }
        else if (auto const late = since("input_late"); late > 0)
        {
            out.essence = Status{PartiallyHealthy, std::to_string(late) + " written late, without input in time"};
        }
        else
        {
            out.essence = Status{Healthy, ""};
        }
        return out;
    }

    // The transmission error counters: every write result but ok, counted
    // since baseline, the sample at the last reset. A writer started again
    // counts from zero, below its baseline.
    inline std::vector<std::pair<std::string, std::uint64_t>> errorCounters(Sample const& current, Sample const& baseline)
    {
        auto out = std::vector<std::pair<std::string, std::uint64_t>>{};
        for (auto const& [result, now] : current.writes)
        {
            if (result == "ok")
            {
                continue;
            }
            auto const before = baseline.writes.contains(result) ? baseline.writes.at(result) : 0;
            out.emplace_back(result, now >= before ? now - before : now);
        }
        return out;
    }
}
