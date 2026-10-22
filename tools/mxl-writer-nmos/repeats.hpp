// Holds back a log line that repeats. NvNmos reports every failed attempt to
// find or reach a registry, several times a second, for as long as the
// failure lasts; written out each time, that buries everything else in the
// log. Links neither libmxl nor NvNmos, so senders-test.cpp covers it.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace repeats
{
    // The first occurrence of a line is printed, then the line is held back
    // for a window after that print; the first occurrence after the window is
    // printed again with how many were held back since the last print. A line
    // that stops repeating gets one last count once it has not been seen for
    // a window, so the log shows how long it went on.
    class Limiter
    {
    public:
        struct Summary
        {
            std::string line;
            std::uint64_t heldBack;
            std::int64_t lastSeenNs;
        };

        static constexpr std::size_t MaxEntries = 256;

        explicit Limiter(std::int64_t windowNs)
            : _windowNs{windowNs}
        {}

        // Whether to print line at nowNs. When true, heldBack is how many
        // occurrences were not printed since its last print.
        bool admit(std::string const& line, std::int64_t nowNs, std::uint64_t& heldBack)
        {
            // Lines that differ in an id or a time never repeat; forgetting
            // them all now and then keeps the table small.
            if (_entries.size() >= MaxEntries && _entries.find(line) == _entries.end())
            {
                _entries.clear();
            }
            auto& entry = _entries[line];
            entry.lastSeenNs = nowNs;
            if (entry.printed && nowNs - entry.lastPrintNs < _windowNs)
            {
                ++entry.heldBack;
                heldBack = 0;
                return false;
            }
            heldBack = entry.heldBack;
            entry.printed = true;
            entry.lastPrintNs = nowNs;
            entry.heldBack = 0;
            return true;
        }

        // The lines held back that have not been seen for a window: their
        // last count, which is then cleared.
        std::vector<Summary> stopped(std::int64_t nowNs)
        {
            auto out = std::vector<Summary>{};
            for (auto& [line, entry] : _entries)
            {
                if (entry.heldBack > 0 && nowNs - entry.lastSeenNs >= _windowNs)
                {
                    out.push_back(Summary{line, entry.heldBack, entry.lastSeenNs});
                    entry.heldBack = 0;
                }
            }
            return out;
        }

    private:
        struct Entry
        {
            bool printed = false;
            std::int64_t lastPrintNs = 0;
            std::int64_t lastSeenNs = 0;
            std::uint64_t heldBack = 0;
        };

        std::int64_t _windowNs;
        std::unordered_map<std::string, Entry> _entries;
    };
}
