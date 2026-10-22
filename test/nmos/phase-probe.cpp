// Reads a ball video flow and a ticks audio flow as a consumer does, and
// prints where in the media second each of them marks: the time at which the
// ball crosses the middle of the frame moving right, and the samples at
// which a tick starts. The lane compares the two, and the same flows across
// restarts.
//
//   mxl-phase-probe <domain> <video flow id or -> <audio flow id or -> <seconds>
//
// One JSON line: {"crossings": [ms, ...], "ticks": [ms, ...]}, each the
// offset of the mark into its TAI second.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <optional>
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

namespace
{
    constexpr std::uint64_t Second = 1'000'000'000ULL;

    double phaseMs(std::uint64_t timestamp)
    {
        return static_cast<double>(timestamp % Second) / 1e6;
    }

    std::uint32_t word(std::uint8_t const* p)
    {
        return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
               static_cast<std::uint32_t>(p[3]) << 24;
    }

    // Luma of pixel x in a v210 line: six pixels per 16 bytes.
    int luma(std::uint8_t const* line, int x)
    {
        static constexpr int Word[] = {0, 1, 1, 2, 3, 3};
        static constexpr int Shift[] = {10, 0, 20, 10, 0, 20};
        auto const* group = line + (x / 6) * 16;
        return static_cast<int>((word(group + 4 * Word[x % 6]) >> Shift[x % 6]) & 0x3ff);
    }

    // The ball's horizontal centre: the midpoint of every run of bright
    // pixels at least 30 long, which only the ball's middle rows have, not
    // the strokes of the overlay text. Nothing where no such run is found.
    std::optional<double> ballX(std::uint8_t const* grain, int width, int height)
    {
        auto const stride = (width + 47) / 48 * 128;
        auto sum = 0.0;
        auto count = 0;
        for (auto y = 0; y < height; ++y)
        {
            auto const* line = grain + static_cast<std::size_t>(y) * stride;
            auto run = 0;
            for (auto x = 0; x <= width; ++x)
            {
                if (x < width && luma(line, x) > 512)
                {
                    ++run;
                    continue;
                }
                if (run >= 30)
                {
                    sum += x - 1 - (run - 1) / 2.0;
                    ++count;
                }
                run = 0;
            }
        }
        return count > 0 ? std::optional<double>{sum / count} : std::nullopt;
    }

    int definitionInt(mxlInstance instance, char const* id, char const* key)
    {
        char text[8192];
        auto size = sizeof(text);
        if (mxlGetFlowDef(instance, id, text, &size) != MXL_STATUS_OK)
        {
            return 0;
        }
        auto const definition = std::string{text, size};
        auto const at = definition.find(std::string{"\""} + key + "\"");
        return at == std::string::npos ? 0 : std::atoi(definition.c_str() + definition.find(':', at) + 1);
    }

    // The ball pattern's centre moves as sin(2 pi n / 200) across the frame,
    // n the source's frame counter, so it crosses the middle moving right
    // once every 200 frames, at n = 0 mod 200. Each such crossing, placed
    // between the two grains it falls between.
    std::vector<double> crossings(mxlInstance instance, char const* id, double seconds)
    {
        auto out = std::vector<double>{};
        auto reader = mxlFlowReader{};
        if (mxlCreateFlowReader(instance, id, "", &reader) != MXL_STATUS_OK)
        {
            std::fprintf(stderr, "cannot open video flow %s\n", id);
            return out;
        }
        auto const width = definitionInt(instance, id, "frame_width");
        auto const height = definitionInt(instance, id, "frame_height");
        auto config = mxlFlowConfigInfo{};
        mxlFlowReaderGetConfigInfo(reader, &config);
        auto const rate = config.common.grainRate;
        auto index = mxlGetCurrentIndex(&rate);
        auto const end = index + static_cast<std::uint64_t>(seconds * rate.numerator / rate.denominator);
        auto previous = std::optional<double>{};
        for (; index < end; ++index)
        {
            auto info = mxlGrainInfo{};
            auto* payload = static_cast<std::uint8_t*>(nullptr);
            if (mxlFlowReaderGetGrain(reader, index, Second, &info, &payload) != MXL_STATUS_OK || payload == nullptr ||
                (info.flags & MXL_GRAIN_FLAG_INVALID) != 0)
            {
                previous.reset();
                continue;
            }
            auto const x = ballX(payload, width, height);
            auto const middle = width / 2.0;
            if (previous && x && *previous < middle && *x >= middle)
            {
                auto const fraction = (middle - *previous) / (*x - *previous);
                auto const at = static_cast<double>(mxlIndexToTimestamp(&rate, index - 1)) +
                                fraction * static_cast<double>(mxlIndexToTimestamp(&rate, index) - mxlIndexToTimestamp(&rate, index - 1));
                out.push_back(std::fmod(at, 1e9) / 1e6);
            }
            previous = x;
        }
        mxlReleaseFlowReader(instance, reader);
        return out;
    }

    // A tick is a burst of sine after silence; it starts at the first sample
    // that is not silent after at least 10 ms that are.
    std::vector<double> ticks(mxlInstance instance, char const* id, double seconds)
    {
        auto out = std::vector<double>{};
        auto reader = mxlFlowReader{};
        if (mxlCreateFlowReader(instance, id, "", &reader) != MXL_STATUS_OK)
        {
            std::fprintf(stderr, "cannot open audio flow %s\n", id);
            return out;
        }
        auto config = mxlFlowConfigInfo{};
        mxlFlowReaderGetConfigInfo(reader, &config);
        auto const rate = config.common.grainRate;
        auto const quiet = static_cast<std::uint64_t>(rate.numerator / rate.denominator / 100);
        constexpr std::size_t Chunk = 480;
        auto index = mxlGetCurrentIndex(&rate);
        auto const end = index + static_cast<std::uint64_t>(seconds * rate.numerator / rate.denominator);
        auto silent = std::uint64_t{0};
        for (; index < end; index += Chunk)
        {
            auto slices = mxlWrappedMultiBufferSlice{};
            // The index is the exclusive end of the range read.
            if (mxlFlowReaderGetSamples(reader, index + Chunk, Chunk, Second, &slices) != MXL_STATUS_OK)
            {
                silent = 0;
                continue;
            }
            auto sample = index;
            for (auto const& fragment : slices.base.fragments)
            {
                auto const* values = static_cast<float const*>(fragment.pointer);
                for (std::size_t i = 0; i < fragment.size / sizeof(float); ++i, ++sample)
                {
                    if (std::fabs(values[i]) < 1e-6F)
                    {
                        ++silent;
                        continue;
                    }
                    if (silent >= quiet)
                    {
                        // The first sample of a tick is sin(0), so the burst
                        // started one sample before the first loud one.
                        out.push_back(phaseMs(mxlIndexToTimestamp(&rate, sample - 1)));
                    }
                    silent = 0;
                }
            }
        }
        mxlReleaseFlowReader(instance, reader);
        return out;
    }

    void print(char const* name, std::vector<double> const& values)
    {
        std::printf("\"%s\":[", name);
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            std::printf("%s%.2f", i == 0 ? "" : ",", values[i]);
        }
        std::printf("]");
    }
}

int main(int argc, char** argv)
{
    if (argc != 5)
    {
        std::fprintf(stderr, "usage: %s <domain> <video flow id or -> <audio flow id or -> <seconds>\n", argv[0]);
        return 2;
    }
    auto const seconds = std::atof(argv[4]);
    auto* instance = mxlCreateInstance(argv[1], "");
    if (instance == nullptr)
    {
        std::fprintf(stderr, "cannot open domain %s\n", argv[1]);
        return 1;
    }
    auto const video = std::string{argv[2]} != "-" ? crossings(instance, argv[2], seconds) : std::vector<double>{};
    auto const audio = std::string{argv[3]} != "-" ? ticks(instance, argv[3], seconds) : std::vector<double>{};
    mxlDestroyInstance(instance);
    std::printf("{");
    print("crossings", video);
    std::printf(",");
    print("ticks", audio);
    std::printf("}\n");
    return 0;
}
