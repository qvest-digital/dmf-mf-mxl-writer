// Tests of what mxl-writer-nmos works out before it starts a writer or the
// Node. Built and run by the image build, so an image that would advertise a
// flow it does not write, or run a writer with another flow's options, is
// never published.

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>
#include <unistd.h>
#include "repeats.hpp"
#include "senders.hpp"

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

        auto bounded = repeats::Limiter{60 * Second};
        for (std::size_t i = 0; i < 10 * repeats::Limiter::MaxEntries; ++i)
        {
            bounded.admit("line " + std::to_string(i), 0, held);
        }
        check(bounded.admit("line 0", Second, held), "a table full of lines that never repeat is forgotten, not grown");
    }
}

int main()
{
    repeating();
    splitting();
    uuids();
    domains();
    definitions();
    if (failures == 0)
    {
        std::fprintf(stderr, "all checks pass\n");
    }
    return failures == 0 ? 0 : 1;
}
