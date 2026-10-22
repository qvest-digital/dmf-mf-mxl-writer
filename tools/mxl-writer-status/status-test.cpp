// Tests of what mxl-writer-status works out about a flow and how it reports
// it. Built and run by the image build, so an image that misreports a ring is
// never published.

#include <chrono>
#include <cstdio>
#include <atomic>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "server.hpp"
#include "status.hpp"

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

    constexpr std::uint64_t Ms = 1'000'000ULL;

    // The shape the mxl-writer chart renders for its video flow.
    constexpr char const* Video = R"({"id": "d4d00000-0000-0000-0000-100000000000", "label": "Video output",
        "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["mxl-multiviewer-1:Video"]}, "media_type": "video/v210",
        "grain_rate": {"numerator": 50, "denominator": 1}, "frame_width": 1920, "frame_height": 1080,
        "interlace_mode": "progressive"})";

    mxlFlowConfigInfo videoConfig()
    {
        auto config = mxlFlowConfigInfo{};
        config.common.format = MXL_DATA_FORMAT_VIDEO;
        config.common.grainRate = {50, 1};
        config.common.maxCommitBatchSizeHint = 1080;
        config.discrete.grainCount = 250;
        // One v210 line of 1920 pixels: 40 blocks of 48 pixels, 128 bytes each.
        config.discrete.sliceSizes[0] = 5120;
        return config;
    }

    mxlFlowConfigInfo audioConfig()
    {
        auto config = mxlFlowConfigInfo{};
        config.common.format = MXL_DATA_FORMAT_AUDIO;
        config.common.grainRate = {48000, 1};
        config.common.maxCommitBatchSizeHint = 480;
        config.continuous.channelCount = 2;
        config.continuous.bufferLength = 240000;
        return config;
    }

    std::uint64_t mediaTime(mxlRational rate, std::uint64_t index)
    {
        return mxlIndexToTimestamp(&rate, index);
    }

    void definitions()
    {
        auto const video = status::parseDefinition(Video);
        check(video && video->mediaType == "video/v210" && video->width == 1920 && video->height == 1080 && !video->interlaced,
            "a v210 definition is read");
        check(video && video->rate == "50/1" && video->label == "Video output", "its rate and label are kept");
        check(video && video->groupHint == "mxl-multiviewer-1:Video", "the group hint is the first grouphint tag");

        auto const audio = status::parseDefinition(R"({"media_type": "audio/float32", "sample_rate": {"numerator": 48000},
            "channel_count": 2})");
        check(audio && audio->sampleRate == 48000 && audio->channels == 2 && audio->groupHint.empty(), "an audio definition is read");
        check(!status::parseDefinition("not json"), "an unreadable definition is no definition");

        auto const huge = status::parseDefinition(R"({"frame_width": 1e300, "frame_height": -1e300, "channel_count": 4294967296})");
        check(huge && huge->width == std::numeric_limits<int>::max() && huge->height == std::numeric_limits<int>::min() &&
                  huge->channels == std::numeric_limits<int>::max(),
            "a number outside int is clamped, not converted");
    }

    void rings()
    {
        auto const video = videoConfig();
        auto declared = status::parseDefinition(Video).value();
        check(status::ringLength(video) == 250 && std::string{status::unit(video)} == "grains", "a video ring counts grains");
        check(status::slotBytes(video, declared) == 5120ULL * 1080, "a grain is one line's slice size times the lines, not one slice");
        declared.interlaced = true;
        check(status::slotBytes(video, declared) == 5120ULL * 540, "an interlaced grain carries one field");
        auto alpha = video;
        alpha.discrete.sliceSizes[1] = 2560;
        declared.interlaced = false;
        check(status::slotBytes(alpha, declared) == (5120ULL + 2560) * 1080, "every plane of a grain counts");

        auto data = video;
        data.common.format = MXL_DATA_FORMAT_DATA;
        data.discrete.sliceSizes[0] = 1;
        check(status::slotBytes(data, declared) == 0, "a data grain has no fixed size to report");

        auto const audio = audioConfig();
        check(status::ringLength(audio) == 240000 && std::string{status::unit(audio)} == "samples", "an audio ring counts samples");
        check(status::slotBytes(audio, status::Declared{}) == 8, "an audio slot is one float sample of every channel");

        check(status::graceNs(video) == 20 * Ms + 50 * Ms, "a grain is due a reader's grace of one period plus 50 ms");
        check(status::graceNs(audio) == 480 * status::periodNs(audio.common.grainRate) + 50 * Ms,
            "samples are due the larger of ten milliseconds and a commit batch, plus 50 ms");
    }

    void tracking()
    {
        auto const rate = mxlRational{50, 1};
        auto tracker = status::Tracker{.continuous = false, .rate = rate, .timeoutNs = 100 * Ms};
        auto const h = std::uint64_t{88'000'000'000};
        auto const t = mediaTime(rate, h);

        check(!tracker.observe(0, 0, t), "a head of zero is nothing committed yet");
        check(!tracker.observe(MXL_UNDEFINED_INDEX, 0, t), "an undefined head is nothing committed yet");
        check(!tracker.observe(h, t + 5 * Ms, t + 6 * Ms), "the first head seen is a baseline, not a commit of unknown size");

        auto const next = tracker.observe(h + 1, mediaTime(rate, h + 1) + 5 * Ms, t + 26 * Ms);
        check(next && next->kind == 'k' && next->step == 1, "the head moving on by one grain is a commit");
        check(next && next->afterMediaNs == static_cast<std::int64_t>(5 * Ms), "a commit is timed by the flow's write time against its media time");
        check(next && next->atNs == mediaTime(rate, h + 1) + 5 * Ms, "a commit happened when the flow says, not when it was seen");
        check(!tracker.observe(h + 1, mediaTime(rate, h + 1) + 5 * Ms, t + 30 * Ms), "an unchanged header is no commit");

        auto const stall = tracker.observe(h + 1, mediaTime(rate, h + 1) + 5 * Ms, t + 26 * Ms + 100 * Ms);
        check(stall && stall->kind == 't' && tracker.stalls == 1, "a whole timeout without a commit is a stall");
        check(!tracker.observe(h + 1, mediaTime(rate, h + 1) + 5 * Ms, t + 400 * Ms) && tracker.stalls == 1, "a stall counts once");

        auto const jump = tracker.observe(h + 4, mediaTime(rate, h + 4), t + 401 * Ms);
        check(jump && jump->kind == 'g' && tracker.skipped == 2 && !tracker.stalled, "a head jumping three grains skipped two");

        auto const again = tracker.observe(h + 4, mediaTime(rate, h + 4) + Ms, t + 402 * Ms);
        check(again && again->kind == 'r' && again->step == 0, "the same index written again is a rewrite");

        // The clock steps back by a second; that is no time without a commit.
        auto const stepped = tracker.observe(h + 4, mediaTime(rate, h + 4) + Ms, t + 402 * Ms - status::NsPerSecond);
        check(!stepped && tracker.stalls == 1, "a clock stepped back is not a stall");
        check(!tracker.observe(h + 4, mediaTime(rate, h + 4) + Ms, t + 402 * Ms - status::NsPerSecond + 50 * Ms) && tracker.stalls == 1,
            "and the wait for one starts again from where the clock is");

        auto const back = tracker.observe(h - 100, mediaTime(rate, h + 5), t + 403 * Ms);
        check(back && back->kind == 'b' && back->step == -104 && tracker.backwards == 1, "a head moving backwards is counted");
        check(back && back->afterMediaNs > 0, "a head stepped back was committed long after its media time");
        check(tracker.commits == 4, "every change of head or write time is a commit");

        // libmxl's continuous writer moves the head and leaves lastWriteTime
        // at the flow's creation.
        auto const audioRate = mxlRational{48000, 1};
        auto audio = status::Tracker{.continuous = true, .rate = audioRate};
        auto const s = std::uint64_t{80'000'000'000'000};
        auto const created = mediaTime(audioRate, s) - 5 * status::NsPerSecond;
        audio.observe(s, created, mediaTime(audioRate, s));
        auto const batch = audio.observe(s + 480, created, mediaTime(audioRate, s + 480) + 3 * Ms);
        check(batch && batch->kind == 'k' && batch->step == 480 && audio.skipped == 0, "a batch of samples is one commit, not skipped samples");
        check(batch && batch->atNs == mediaTime(audioRate, s + 480) + 3 * Ms && audio.committedAt == batch->atNs,
            "a sample commit is timed when it was seen, not by a write time libmxl never stamps");
        check(batch && batch->afterMediaNs == static_cast<std::int64_t>(3 * Ms), "and is timed against the media time of its end");
        check(!audio.observe(s + 480, created, mediaTime(audioRate, s + 480) + 4 * Ms), "a sample head that did not move is no commit");
    }

    void rates()
    {
        // Commits every 20 ms on the flow's clock.
        auto events = std::deque<status::Commit>{};
        for (std::uint64_t i = 0; i < 100; ++i)
        {
            events.push_back({.head = 1000 + i, .atNs = 5'000 * Ms + i * 20 * Ms, .step = 1, .kind = 'k'});
        }
        auto const rate = status::headRate(events, status::NsPerSecond);
        check(rate && *rate > 49.999 && *rate < 50.001, "the head rate comes from the flow's head and clock, so it is exact");

        events.push_back({.head = 1100, .atNs = events.back().atNs + 200 * Ms, .kind = 't'});
        check(status::headRate(events, status::NsPerSecond) == rate, "a stall adds nothing to the rate");

        events.push_back({.head = 900, .atNs = events.back().atNs + 20 * Ms, .step = -199, .kind = 'b'});
        check(!status::headRate(events, status::NsPerSecond), "no rate is measured across a backwards step");
        events.push_back({.head = 901, .atNs = events.back().atNs + 40 * Ms, .step = 1, .kind = 'k'});
        auto const after = status::headRate(events, status::NsPerSecond);
        check(after && *after > 24.999 && *after < 25.001, "after a backwards step the rate is measured from it");

        check(!status::headRate({}, status::NsPerSecond), "no commits, no rate");
        check(!status::headRate(
                  {
                      {.head = 5, .atNs = 1}
        },
                  status::NsPerSecond),
            "one commit, no rate");
    }

    status::FlowView openVideo(std::uint64_t head, std::uint64_t writeNs)
    {
        auto view = status::FlowView{};
        view.id = "d4d00000-0000-0000-0000-100000000000";
        view.state = "open";
        view.open = true;
        view.unit = "grains";
        view.mediaType = "video/v210";
        view.label = "Video output";
        view.format = "1920x1080 @ 50/1";
        view.rate = {50, 1};
        view.ringLength = 250;
        view.slotBytes = 5120ULL * 1080;
        view.periodNs = 20 * Ms;
        view.head = head;
        view.writeNs = writeNs;
        view.readNs = writeNs - 2 * status::NsPerSecond;
        view.headRate = 50.0;
        view.commits = 12;
        return view;
    }

    picojson::object flowJson(std::string const& text, std::size_t index)
    {
        auto json = picojson::value{};
        if (!picojson::parse(json, text).empty() || !json.is<picojson::object>())
        {
            return {};
        }
        auto const& flows = json.get<picojson::object>().at("flows").get<picojson::array>();
        return index < flows.size() ? flows[index].get<picojson::object>() : picojson::object{};
    }

    void reopening()
    {
        auto const old = status::FileIdentity{.device = 41, .inode = 7};
        check(!status::replaced(old, old), "the same data file is the same flow");
        check(status::replaced(old, status::FileIdentity{.device = 41, .inode = 8}), "another data file is a flow created anew");
        check(status::replaced(old, std::nullopt), "no data file is a flow removed");
        check(status::replaced(std::nullopt, old),
            "a flow whose file could not be read before it opened is reopened once its file is there, so a replacement in between is not missed");
    }

    void requests()
    {
        using Outcome = http::Request::Outcome;
        auto const get = http::parseRequest("GET /status?x=1 HTTP/1.1\r\nHost: a\r\n\r\n");
        check(get.outcome == Outcome::Complete && get.method == "GET" && get.path == "/status", "a request's path is read without its query");
        check(http::parseRequest("GET /status HTTP/1.1\r\nHost: a\r\n").outcome == Outcome::Incomplete,
            "a header without its blank line is not served yet");
        check(http::parseRequest("GET /sta").outcome == Outcome::Incomplete, "nor is half a request line");
        check(http::parseRequest(std::string(http::MaxRequestBytes, 'a')).outcome == Outcome::TooLarge,
            "a header that never ends within the cap is refused");
        check(http::parseRequest("GET / HTTP/1.1\r\nX: " + std::string(http::MaxRequestBytes, 'a') + "\r\n\r\n").outcome == Outcome::TooLarge,
            "and so is one that ends past it");
        check(http::parseRequest("GET\r\n\r\n").outcome == Outcome::Bad, "a request line without a target is bad");
        check(http::parseRequest("GET status HTTP/1.1\r\n\r\n").outcome == Outcome::Bad, "a target must be a path");
        check(http::parseRequest("GET / SPDY/3\r\n\r\n").outcome == Outcome::Bad, "only HTTP/1 is served");
        auto const post = http::parseRequest("POST /status HTTP/1.1\r\n\r\n");
        check(post.outcome == Outcome::Complete && post.method == "POST", "the method is kept for the server to refuse");
    }

    int connectTo(std::uint16_t port)
    {
        auto const fd = ::socket(AF_INET, SOCK_STREAM, 0);
        auto addr = sockaddr_in{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    // Sends request whole or in pieces, and returns everything until the
    // server closes.
    std::string exchange(std::uint16_t port, std::vector<std::string> const& pieces)
    {
        auto const fd = connectTo(port);
        if (fd < 0)
        {
            return {};
        }
        auto const tv = timeval{.tv_sec = 3, .tv_usec = 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        for (auto const& piece : pieces)
        {
            ::send(fd, piece.data(), piece.size(), MSG_NOSIGNAL);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        auto out = std::string{};
        char buffer[65536];
        while (true)
        {
            auto const n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0)
            {
                break;
            }
            out.append(buffer, static_cast<std::size_t>(n));
        }
        ::close(fd);
        return out;
    }

    void serving()
    {
        auto stop = std::atomic<bool>{false};
        auto server = http::Server{};
        auto const big = std::string(4 << 20, 'x');
        server.route = [&big](std::string const& path) -> http::Response
        {
            if (path == "/healthz")
            {
                return {.body = "ok\n"};
            }
            if (path == "/big")
            {
                return {.body = big};
            }
            if (path == "/stream")
            {
                return {.stream = true};
            }
            return {.code = 404, .body = "not found\n"};
        };
        server.tick = [](std::vector<std::uint64_t>& cursors)
        {
            cursors.push_back(0);
            return fmt::format("{{\"n\":{}}}", cursors.size());
        };
        server.requestTimeout = std::chrono::milliseconds{1500};
        auto const port = server.listen(0, true);
        check(port.has_value(), "the server listens");
        if (!port)
        {
            return;
        }
        auto thread = std::thread{[&]
            {
                server.run(stop);
            }};

        // Twelve clients that connect and send nothing, and one that sends
        // half a request line and stops.
        auto idle = std::vector<int>{};
        for (auto i = 0; i < 12; ++i)
        {
            idle.push_back(connectTo(*port));
        }
        auto const half = connectTo(*port);
        ::send(half, "GET /hea", 8, MSG_NOSIGNAL);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto const started = std::chrono::steady_clock::now();
        auto const health = exchange(*port, {"GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n"});
        auto const took = std::chrono::steady_clock::now() - started;
        check(health.starts_with("HTTP/1.1 200 OK\r\n") && health.ends_with("\r\n\r\nok\n"), "/healthz answers past idle connections");
        check(took < std::chrono::milliseconds{100}, "and within 100 ms, well inside a probe's timeout");
        std::fprintf(stderr, "healthz with 13 idle connections open: %.1f ms\n", std::chrono::duration<double, std::milli>(took).count());

        check(exchange(*port, {"GET /hea", "lthz HTTP/1.1\r\n", "Host: x\r\n\r\n"}).ends_with("ok\n"),
            "a request sent in pieces is served once its header ends");
        check(exchange(*port, {"GET / HTTP/1.1\r\nX: " + std::string(http::MaxRequestBytes, 'a')}).starts_with("HTTP/1.1 431 "),
            "a header past the cap is refused with 431");
        check(exchange(*port, {"nonsense\r\n\r\n"}).starts_with("HTTP/1.1 400 "), "a malformed request line gets 400");
        check(exchange(*port, {"POST /healthz HTTP/1.1\r\n\r\n"}).starts_with("HTTP/1.1 405 "), "anything but GET gets 405");
        check(exchange(*port, {"GET /nope HTTP/1.1\r\n\r\n"}).starts_with("HTTP/1.1 404 "), "an unknown path gets 404");

        auto const whole = exchange(*port, {"GET /big HTTP/1.1\r\n\r\n"});
        check(whole.size() > big.size() && whole.ends_with(big), "a response larger than the socket buffer is sent in full");

        // A stream answers with its header and a batch at once, then one
        // batch per interval, each with the cursors it was handed.
        auto const viewer = connectTo(*port);
        ::send(viewer, "GET /stream HTTP/1.1\r\n\r\n", 26, MSG_NOSIGNAL);
        std::this_thread::sleep_for(std::chrono::milliseconds{180});
        auto const shortTv = timeval{.tv_sec = 0, .tv_usec = 200'000};
        ::setsockopt(viewer, SOL_SOCKET, SO_RCVTIMEO, &shortTv, sizeof(shortTv));
        char streamBuffer[8192];
        auto const got = ::recv(viewer, streamBuffer, sizeof(streamBuffer), 0);
        auto const streamed = std::string{streamBuffer, got > 0 ? static_cast<std::size_t>(got) : 0U};
        check(streamed.starts_with("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"), "a stream is answered with an event stream");
        check(streamed.find("data: {\"n\":1}\n\n") != std::string::npos && streamed.find("data: {\"n\":3}\n\n") != std::string::npos,
            "and fed a batch every interval with its own cursors");
        auto const healthDuringStream = exchange(*port, {"GET /healthz HTTP/1.1\r\n\r\n"});
        check(healthDuringStream.ends_with("ok\n"), "a stream holds up no other request");
        ::close(viewer);

        // The idle clients are dropped once their request timeout passes.
        std::this_thread::sleep_for(std::chrono::milliseconds{1700});
        char byte;
        auto const tv = timeval{.tv_sec = 1, .tv_usec = 0};
        ::setsockopt(idle.front(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        check(::recv(idle.front(), &byte, 1, 0) == 0, "a client that never completes its request is closed");

        stop = true;
        thread.join();
        for (auto const fd : idle)
        {
            ::close(fd);
        }
        ::close(half);
    }

    void rendering()
    {
        auto const rate = mxlRational{50, 1};
        auto const head = std::uint64_t{88'000'000'000};
        // Three grains ahead of the media clock, last written 4 ms ago.
        auto const now = mediaTime(rate, head - 3) + Ms;
        auto video = openVideo(head, now - 4 * Ms);
        auto waiting = status::FlowView{};
        waiting.id = "d4d00000-0000-0000-0000-100000000001";
        waiting.state = "flow_not_found";
        waiting.lastError = "flow not found in the domain";
        waiting.lastErrorAt = now - 3 * status::NsPerSecond;

        auto const text = status::statusJson("mxl-writer-status test", {video, waiting}, now);
        auto const f = flowJson(text, 0);
        check(!f.empty(), "/status is JSON with one object per flow");
        check(!f.empty() && f.at("writing").get<bool>(), "a flow written 4 ms ago is writing");
        check(!f.empty() && f.at("headVsNow").get<double>() == 3.0, "a head three grains past the media clock leads by three");
        check(!f.empty() && f.at("headVsNowMs").get<double>() == 60.0, "the lead is also given in milliseconds");
        check(!f.empty() && f.at("ring").get<picojson::object>().at("seconds").get<double>() == 5.0, "250 grains at 50/1 hold five seconds");
        check(!f.empty() && f.at("ring").get<picojson::object>().at("bytes").get<double>() == 250.0 * 5120 * 1080, "the ring's bytes are its slots'");
        check(!f.empty() && f.at("readAgeSeconds").get<double>() > 2.0, "the last read time libmxl records is reported");
        check(!f.empty() && f.at("label").get<std::string>() == "Video output" && f.at("format").get<std::string>() == "1920x1080 @ 50/1",
            "the label and the format are separate");

        auto audio = openVideo(head, now - 4 * Ms);
        audio.unit = "samples";
        audio.id = "d4d00000-0000-0000-0000-100000000002";
        auto const audioText = status::statusJson("v", {audio}, now);
        auto const a = flowJson(audioText, 0);
        check(!a.empty() && a.find("readAgeSeconds") == a.end(), "samples carry no read age: libmxl keeps none for them");
        check(status::metricsText({audio}, now).find("mxl_writer_status_read_age_seconds{") == std::string::npos, "nor does /metrics give one");

        auto const w = flowJson(text, 1);
        check(!w.empty() && !w.at("writing").get<bool>() && w.find("head") == w.end(), "a flow not found has no head to report");
        check(!w.empty() && w.at("lastError").get<picojson::object>().at("message").get<std::string>() == "flow not found in the domain",
            "the last error is reported");

        auto const stale = status::statusJson("v", {openVideo(head, now - 2 * status::NsPerSecond)}, now);
        check(!flowJson(stale, 0).at("writing").get<bool>(), "a flow last written two seconds ago is not writing");

        auto const metrics = status::metricsText({video, waiting}, now);
        check(metrics.find("mxl_writer_status_head_lead_seconds{flow=\"d4d00000-0000-0000-0000-100000000000\"} 0.060000\n") != std::string::npos,
            "the lead is a metric, in seconds");
        check(metrics.find("mxl_writer_status_head_rate{flow=\"d4d00000-0000-0000-0000-100000000000\"} 50.000000\n") != std::string::npos,
            "the head rate is a metric");
        check(metrics.find("mxl_writer_status_flow_state{flow=\"d4d00000-0000-0000-0000-100000000001\",state=\"flow_not_found\"} 1\n") !=
                  std::string::npos,
            "every flow reports its state");
        check(metrics.find("mxl_writer_status_head_index{flow=\"d4d00000-0000-0000-0000-100000000001\"}") == std::string::npos,
            "a flow not found has no head index metric");
        check(metrics.find("mxl_writer_status_commits_total{flow=\"d4d00000-0000-0000-0000-100000000001\"} 0\n") != std::string::npos,
            "counters exist from the start");
        check(status::labelValue("a\"b\\c\nd") == "a_b_c_d", "quotes, backslashes and newlines cannot break a label");
    }
}

int main()
{
    definitions();
    rings();
    tracking();
    rates();
    rendering();
    reopening();
    requests();
    serving();
    if (failures == 0)
    {
        std::fprintf(stderr, "all checks pass\n");
    }
    return failures == 0 ? 0 : 1;
}
