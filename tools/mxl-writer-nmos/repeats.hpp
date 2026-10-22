// Holds back a log line that repeats. NvNmos reports every failed attempt to
// find or reach a registry, several times a second, for as long as the
// failure lasts; written out each time, that buries everything else in the
// log. Links neither libmxl nor NvNmos, so senders-test.cpp covers it.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace repeats
{
    // The first occurrence of a line is printed, then the line is held back
    // for a window; the first occurrence after the window is printed again
    // with how many were held back.
    class Limiter
    {
    public:
        explicit Limiter(std::int64_t windowNs)
            : _windowNs{windowNs}
        {}

        // Whether to print key at nowNs. When true, heldBack is how many
        // occurrences were not printed since the last time it was.
        bool admit(std::string const& key, std::int64_t nowNs, std::uint64_t& heldBack)
        {
            // Lines that differ in an id or a time never repeat; forgetting
            // them all now and then keeps the table small.
            if (_entries.size() >= MaxEntries && _entries.find(key) == _entries.end())
            {
                _entries.clear();
            }
            auto& entry = _entries[key];
            if (entry.printed && nowNs - entry.lastNs < _windowNs)
            {
                ++entry.heldBack;
                heldBack = 0;
                return false;
            }
            heldBack = entry.heldBack;
            entry = Entry{true, nowNs, 0};
            return true;
        }

        static constexpr std::size_t MaxEntries = 256;

    private:
        struct Entry
        {
            bool printed = false;
            std::int64_t lastNs = 0;
            std::uint64_t heldBack = 0;
        };

        std::int64_t _windowNs;
        std::unordered_map<std::string, Entry> _entries;
    };
}
