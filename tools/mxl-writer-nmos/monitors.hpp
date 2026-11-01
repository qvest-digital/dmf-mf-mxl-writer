// What a Sender's BCP-008 monitor reports. The writer processes serve their
// counts as Prometheus text; this reads them back into libmxl-monitoring
// snapshots, whose bcp008.h judges the statuses and counts the counters.
// Links no NvNmos, so senders-test.cpp covers it on its own.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <mxl-monitoring/bcp008.h>
#include <mxl-monitoring/monitoring.h>

namespace monitors
{
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

        struct Key
        {
            char const* name;
            int index;
        };

        // The label values libmxl-monitoring's Prometheus text gives a
        // writer's write results and open failures.
        constexpr Key Writes[] = {
            {"ok",               MXL_MONITORING_WRITE_OK               },
            {"input_late",       MXL_MONITORING_WRITE_INPUT_LATE       },
            {"invalid_input",    MXL_MONITORING_WRITE_INVALID_INPUT    },
            {"permission_error", MXL_MONITORING_WRITE_PERMISSION_DENIED},
            {"out_of_space",     MXL_MONITORING_WRITE_OUT_OF_SPACE     },
            {"other_error",      MXL_MONITORING_WRITE_ERROR            },
        };
        constexpr Key OpenFailures[] = {
            {"domain",     MXL_MONITORING_ACCESS_DOMAIN_UNAVAILABLE},
            {"not_found",  MXL_MONITORING_ACCESS_FLOW_NOT_FOUND    },
            {"permission", MXL_MONITORING_ACCESS_PERMISSION_DENIED },
            {"other",      MXL_MONITORING_ACCESS_OTHER             },
        };

        template<std::size_t N>
        std::optional<int> find(Key const (&keys)[N], std::string_view name)
        {
            for (auto const& key : keys)
            {
                if (name == key.name)
                {
                    return key.index;
                }
            }
            return std::nullopt;
        }
    }

    using Snapshot = mxlMonitoringFlowSnapshot;

    // The snapshot of the writer of flowId in a writer process's Prometheus
    // text, or nothing where the text names no state for it. Holds what the
    // text carries: access, delivery, unit and every count.
    inline std::optional<Snapshot> parse(std::string_view text, std::string const& flowId)
    {
        auto s = Snapshot{};
        s.version = 2;
        s.size = sizeof(s);
        s.role = MXL_MONITORING_ROLE_WRITER;
        std::strncpy(s.flowId, flowId.c_str(), sizeof(s.flowId) - 1);
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
            auto const value = std::strtoull(std::string{line.substr(close + 1)}.c_str(), nullptr, 10);
            if (name == "mxl_writer_flow_state" && value == 1)
            {
                for (auto access = 0; access < MXL_MONITORING_ACCESS_COUNT; ++access)
                {
                    if (details::label(labels, "state") == mxlMonitoringAccessName(static_cast<mxlMonitoringAccess>(access)))
                    {
                        s.access = static_cast<mxlMonitoringAccess>(access);
                        stated = true;
                        break;
                    }
                }
            }
            else if (name == "mxl_writer_flow_delivering")
            {
                s.delivering = value == 1;
            }
            else if (name == "mxl_writer_flow_info")
            {
                s.unit = details::label(labels, "unit") == "samples" ? MXL_MONITORING_UNIT_SAMPLE : MXL_MONITORING_UNIT_GRAIN;
            }
            else if (name == "mxl_writer_writes_total")
            {
                if (auto const i = details::find(details::Writes, details::label(labels, "result").value_or("")))
                {
                    s.writes[*i] = value;
                }
            }
            else if (name == "mxl_writer_invalid_input_total")
            {
                for (auto issue = 0; issue < MXL_MONITORING_CONTENT_COUNT; ++issue)
                {
                    if (details::label(labels, "issue") == mxlMonitoringContentName(static_cast<mxlMonitoringContent>(issue)))
                    {
                        s.content[issue] = value;
                    }
                }
            }
            else if (name == "mxl_writer_open_failures_total")
            {
                if (auto const i = details::find(details::OpenFailures, details::label(labels, "reason").value_or("")))
                {
                    s.accessFailures[*i] = value;
                }
            }
            else if (name == "mxl_writer_opens_total")
            {
                s.opens = value;
            }
            else if (name == "mxl_writer_lost_total")
            {
                s.losses = value;
            }
        }
        return stated ? std::optional{s} : std::nullopt;
    }

    // Link, transmission, synchronization and essence, in that order.
    using Statuses = std::array<mxlMonitoringBcp008Domain, 4>;

    // The statuses of an active Sender from its writer's snapshot now, or
    // none where the writer process does not answer, and the snapshot a poll
    // before. A domain left UNCHANGED keeps what it reported.
    inline Statuses judge(std::optional<Snapshot> const& previous, std::optional<Snapshot> const& current)
    {
        auto const unchanged = mxlMonitoringBcp008Domain{.status = MXL_MONITORING_BCP008_UNCHANGED, .message = {}};
        auto out = Statuses{unchanged, unchanged, unchanged, unchanged};
        auto statuses = mxlMonitoringBcp008Statuses{};
        statuses.version = 1;
        statuses.size = sizeof(statuses);
        if (!current)
        {
            out[1].status = MXL_MONITORING_BCP008_UNHEALTHY;
            std::strncpy(out[1].message, "the writer process does not answer", sizeof(out[1].message) - 1);
        }
        else if (mxlMonitoringBcp008GetStatuses(previous ? &*previous : nullptr, &*current, &statuses) == MXL_STATUS_OK)
        {
            out = {statuses.link, statuses.connection, statuses.synchronization, statuses.stream};
        }
        return out;
    }

    // The transmission error counters, counted since baseline, the snapshot
    // at the monitor's last reset, or since the writer process started
    // without one.
    inline std::vector<mxlMonitoringBcp008Counter> errorCounters(std::optional<Snapshot> const& baseline, Snapshot const& latest)
    {
        auto out = std::vector<mxlMonitoringBcp008Counter>(4);
        auto count = out.size();
        if (mxlMonitoringBcp008GetCounters(baseline ? &*baseline : nullptr, &latest, MXL_MONITORING_BCP008_TRANSMISSION_ERROR, out.data(),
                &count) != MXL_STATUS_OK)
        {
            return {};
        }
        out.resize(std::min(count, out.size()));
        return out;
    }
}
