// Tests the faults mxl-gst-testsrc writes on command (patch 0013): which
// commands a flow takes, how many grains each spoils, and that a command
// written into the flow's FIFO reaches it. Built and run by the image build.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include "../mxl-gst/faults.hpp"

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

    void parsing()
    {
        auto const invalid = faults::parse("invalid 5", true);
        check(invalid && invalid->kind == faults::Kind::Invalid && invalid->count == 5, "invalid N spoils N grains");
        check(faults::parse("partial 3\r", true)->kind == faults::Kind::Partial, "a line from a CRLF writer is taken");
        check(faults::parse("illegal 1", true)->kind == faults::Kind::Illegal, "illegal N is a grain fault");
        check(faults::parse("size 2", true)->kind == faults::Kind::Size, "size N is a grain fault");
        auto const pause = faults::parse("pause 0.5", false);
        check(pause && pause->kind == faults::Kind::Pause && pause->duration == std::chrono::milliseconds{500}, "an audio flow takes a pause");
        check(!faults::parse("invalid 5", false), "an audio flow refuses a grain fault, which only a grain can carry");
        for (auto const* line : {"", "invalid", "invalid 0", "invalid -1", "invalid 5x", "invalid 100001", "pause 0", "pause -2", "pause 3601",
                 "remove", "reset 1"})
        {
            check(!faults::parse(line, true), (std::string{"refused: '"} + line + "'").c_str());
        }
    }

    void taking()
    {
        auto state = faults::State{};
        check(state.takeGrain() == faults::Kind::None, "without a command no grain is spoiled");
        state.apply(*faults::parse("invalid 2", true));
        check(state.takeGrain() == faults::Kind::Invalid && state.takeGrain() == faults::Kind::Invalid, "the next two grains are invalid");
        check(state.takeGrain() == faults::Kind::None, "the third is written as it is");
        state.apply(*faults::parse("size 5", true));
        state.apply(*faults::parse("illegal 1", true));
        check(state.takeGrain() == faults::Kind::Illegal && state.takeGrain() == faults::Kind::None, "a new grain fault replaces the one under way");
        state.apply(*faults::parse("pause 2", true));
        check(state.takePause() == std::chrono::seconds{2} && state.takePause() == std::chrono::seconds{0}, "a pause is taken once");
        check(state.takeGrain() == faults::Kind::None, "a pause spoils no grain");
    }

    void listening()
    {
        auto const directory = std::filesystem::temp_directory_path() / ("faults-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(directory);
        auto const path = (directory / "5fbec3b1-1b0f-417d-9059-8b94a47197ed").string();
        auto stop = std::atomic<int>{0};
        auto state = faults::State{};
        auto lines = std::vector<std::string>{};
        auto mutex = std::mutex{};
        {
            auto listener = faults::Listener{path, state, true, stop, [&](std::string const& line)
                {
                    auto const lock = std::lock_guard{mutex};
                    lines.push_back(line);
                }};
            check(std::filesystem::is_fifo(path), "the flow's FIFO is named by its id");
            for (auto const* text : {"invalid 3\nbogus 1\n", "pau", "se 1\n"})
            {
                // Each write opens and closes the FIFO, as echo does.
                auto const fd = ::open(path.c_str(), O_WRONLY);
                check(fd >= 0 && ::write(fd, text, std::strlen(text)) == static_cast<ssize_t>(std::strlen(text)), "a command is written");
                ::close(fd);
            }
            for (auto waited = 0; waited < 100; ++waited)
            {
                {
                    auto const lock = std::lock_guard{mutex};
                    if (lines.size() >= 3)
                    {
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
            stop = 1;
        }
        check(lines.size() == 3 && lines[0] == "fault: invalid 3" && lines[1] == "fault refused: 'bogus 1'" && lines[2] == "fault: pause 1",
            "commands are taken line by line across writes, a bad one refused");
        check(state.takeGrain() == faults::Kind::Invalid, "a command through the FIFO reaches the flow");
        check(state.takePause() == std::chrono::seconds{1}, "a command split across writes reaches it whole");
        check(!std::filesystem::exists(path), "the FIFO is removed with its listener");
        std::filesystem::remove_all(directory);
    }
}

int main()
{
    parsing();
    taking();
    listening();
    if (failures == 0)
    {
        std::fprintf(stderr, "all checks pass\n");
    }
    return failures == 0 ? 0 : 1;
}
