// What mxl-writer-nmos works out before it starts anything: which writer
// processes to run, which flows they write, the domain they write into, and
// the MXL flow definition NvNmos takes for each Sender. Links neither libmxl
// nor NvNmos, so senders-test.cpp covers it on its own.

#pragma once

#include <cstdio>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <picojson/wrapper.h>

namespace senders
{
    // Whether s is a lowercase UUID, the form BCP-007-03 gives mxl_flow_id
    // and mxl_domain_id.
    inline bool isUuid(std::string const& s)
    {
        if (s.size() != 36)
        {
            return false;
        }
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            auto const c = s[i];
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (c != '-')
                {
                    return false;
                }
            }
            else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            {
                return false;
            }
        }
        return true;
    }

    // mxl-gst-testsrc's command line, split into one command per flow.
    //
    // The test source writes every flow it is given from one process, so one
    // flow cannot stop while the other runs. Each Sender gets a process of its
    // own instead, running the same binary with the options that apply to its
    // flow plus the ones both share.
    struct Writers
    {
        std::string domain;
        // mxl-gst-testsrc's own default, so a command line without -g names
        // the same group either way.
        std::string groupHint = "mxl-gst-testsrc-group";
        std::string videoDefinition;
        std::string audioDefinition;
        // The port the Node serves the writers' metrics on, 0 for none. Each
        // writer serves its own on loopback, video one port above it and
        // audio two, where the Node collects them.
        unsigned int metricsPort = 0;
        // Arguments after the binary, one list per flow; empty where the
        // flow is not written.
        std::vector<std::string> video;
        std::vector<std::string> audio;
    };

    // Every option mxl-gst-testsrc takes, all of them with a value, and
    // where it goes. A Node option is the Node's own and reaches no writer as
    // given.
    enum class Scope
    {
        Common,
        Video,
        Audio,
        Node,
    };

    struct Option
    {
        char const* shortName;
        char const* longName;
        Scope scope;
    };

    inline constexpr Option Options[] = {
        {"-d", "--domain", Scope::Common},
        {"-g", "--group-hint", Scope::Common},
        {"-v", "--video-config-file", Scope::Video},
        {nullptr, "--video-options-file", Scope::Video},
        {"-p", "--pattern", Scope::Video},
        {"-t", "--overlay-text", Scope::Video},
        {nullptr, "--video-offset", Scope::Video},
        {"-a", "--audio-config-file", Scope::Audio},
        {nullptr, "--audio-options-file", Scope::Audio},
        {nullptr, "--audio-offset", Scope::Audio},
        {nullptr, "--audio-wave", Scope::Audio},
        {nullptr, "--audio-freq", Scope::Audio},
        {nullptr, "--audio-volume", Scope::Audio},
        {nullptr, "--metrics-port", Scope::Node},
    };

    inline Option const* findOption(std::string const& arg)
    {
        for (auto const& option : Options)
        {
            if ((option.shortName != nullptr && arg == option.shortName) || arg == option.longName)
            {
                return &option;
            }
        }
        return nullptr;
    }

    // Splits args, mxl-gst-testsrc's arguments without the program name, or
    // returns nothing with the reason in error. Only the "-x value" and
    // "--name value" forms are taken; anything else is refused rather than
    // passed on to a process that might read it differently.
    inline std::optional<Writers> splitArgs(std::vector<std::string> const& args, std::string& error)
    {
        auto writers = Writers{};
        auto common = std::vector<std::string>{};
        auto video = std::vector<std::string>{};
        auto audio = std::vector<std::string>{};
        for (std::size_t i = 0; i < args.size(); i += 2)
        {
            auto const* option = findOption(args[i]);
            if (option == nullptr)
            {
                error = "unknown argument '" + args[i] + "'";
                return std::nullopt;
            }
            if (i + 1 >= args.size())
            {
                error = args[i] + " needs a value";
                return std::nullopt;
            }
            auto const& value = args[i + 1];
            auto const name = std::string{option->longName};
            if (name == "--domain")
            {
                writers.domain = value;
            }
            else if (name == "--group-hint")
            {
                writers.groupHint = value;
            }
            else if (name == "--video-config-file")
            {
                writers.videoDefinition = value;
            }
            else if (name == "--audio-config-file")
            {
                writers.audioDefinition = value;
            }
            else if (name == "--metrics-port")
            {
                auto port = 0UL;
                for (auto const c : value)
                {
                    if (c < '0' || c > '9' || (port = port * 10 + static_cast<unsigned long>(c - '0')) > 65533)
                    {
                        port = 0;
                        break;
                    }
                }
                // The writers take the two ports above it.
                if (port == 0)
                {
                    error = "--metrics-port '" + value + "' is not a port from 1 to 65533";
                    return std::nullopt;
                }
                writers.metricsPort = static_cast<unsigned int>(port);
                continue;
            }
            auto& into = option->scope == Scope::Common ? common : option->scope == Scope::Video ? video : audio;
            into.push_back(args[i]);
            into.push_back(value);
        }
        if (writers.domain.empty())
        {
            error = "-d, the MXL domain directory, is required";
            return std::nullopt;
        }
        if (writers.videoDefinition.empty() && writers.audioDefinition.empty())
        {
            error = "neither -v nor -a names a flow, so there is nothing to write";
            return std::nullopt;
        }
        // A video option without a video flow is as meaningless to the
        // test source as it is here; dropping it silently would hide that.
        if (writers.videoDefinition.empty() && !video.empty())
        {
            error = video.front() + " is given without -v";
            return std::nullopt;
        }
        if (writers.audioDefinition.empty() && !audio.empty())
        {
            error = audio.front() + " is given without -a";
            return std::nullopt;
        }
        auto metrics = [&](unsigned int offset)
        {
            return writers.metricsPort == 0
                     ? std::vector<std::string>{}
                     : std::vector<std::string>{"--metrics-address", "127.0.0.1", "--metrics-port", std::to_string(writers.metricsPort + offset)};
        };
        if (!writers.videoDefinition.empty())
        {
            writers.video = common;
            writers.video.insert(writers.video.end(), video.begin(), video.end());
            std::ranges::copy(metrics(1), std::back_inserter(writers.video));
        }
        if (!writers.audioDefinition.empty())
        {
            writers.audio = common;
            writers.audio.insert(writers.audio.end(), audio.begin(), audio.end());
            std::ranges::copy(metrics(2), std::back_inserter(writers.audio));
        }
        return writers;
    }

    inline std::optional<picojson::object> parseObject(std::string const& text)
    {
        auto json = picojson::value{};
        if (!picojson::parse(json, text).empty() || !json.is<picojson::object>())
        {
            return std::nullopt;
        }
        return json.get<picojson::object>();
    }

    inline std::optional<std::string> readFile(std::string const& path)
    {
        auto file = std::ifstream{path, std::ios::in | std::ios::binary};
        if (!file)
        {
            return std::nullopt;
        }
        auto text = std::stringstream{};
        text << file.rdbuf();
        return text.str();
    }

    inline std::string textField(picojson::object const& object, char const* key)
    {
        auto const it = object.find(key);
        return it != object.end() && it->second.is<std::string>() ? it->second.get<std::string>() : std::string{};
    }

    // The id of the domain at domainPath, from the domain_def.json the
    // platform writes into every MXL domain, or nothing with the reason in
    // error. A directory named as a UUID has to carry that UUID in its file:
    // the domains/ID layout names each domain after its id, and a mismatch
    // means the file or the mount is wrong, which the id alone cannot tell.
    inline std::optional<std::string> domainId(std::string const& domainPath, std::string& error)
    {
        auto const path = domainPath + "/domain_def.json";
        auto const text = readFile(path);
        if (!text)
        {
            error = path + " cannot be read; without it there is no mxl_domain_id to advertise";
            return std::nullopt;
        }
        auto const object = parseObject(*text);
        auto const id = object ? textField(*object, "id") : std::string{};
        if (id.empty())
        {
            error = path + " has no string \"id\"";
            return std::nullopt;
        }
        if (!isUuid(id))
        {
            error = path + " id \"" + id + "\" is not a lowercase UUID";
            return std::nullopt;
        }
        auto trimmed = domainPath;
        while (trimmed.size() > 1 && trimmed.back() == '/')
        {
            trimmed.pop_back();
        }
        auto const name = std::filesystem::path{trimmed}.filename().string();
        if (isUuid(name) && name != id)
        {
            error = domainPath + " is named for domain " + name + " but its domain_def.json carries " + id;
            return std::nullopt;
        }
        return id;
    }

    // One flow a Sender stands for: its definition as the chart wrote it and
    // the MXL flow id it carries.
    struct Flow
    {
        // "video" or "audio": the name NvNmos derives the Sender's, Flow's
        // and Source's ids from, with the seed. Renaming one moves those ids.
        std::string name;
        std::string flowId;
        picojson::object definition;
    };

    inline std::optional<Flow> readFlow(std::string const& name, std::string const& path, std::string& error)
    {
        auto const text = readFile(path);
        if (!text)
        {
            error = path + " cannot be read";
            return std::nullopt;
        }
        auto const object = parseObject(*text);
        if (!object)
        {
            error = path + " is not a JSON object";
            return std::nullopt;
        }
        auto const id = textField(*object, "id");
        if (!isUuid(id))
        {
            error = path + " id \"" + id + "\" is not a lowercase UUID, and an MXL Sender advertises its flow by one";
            return std::nullopt;
        }
        return Flow{name, id, *object};
    }

    // The role in the Sender's group hint. mxl-gst-testsrc writes "video"
    // and "audio" into the flow itself; NMOS has no standard role names, and
    // the controllers and receivers here group by "Video" and "Audio".
    inline std::string role(Flow const& flow)
    {
        return flow.name == "video" ? "Video" : "Audio";
    }

    // The MXL flow definition NvNmos takes for the Sender: the flow's own,
    // with the Sender's group hint and label, and the NvNmos tags naming the
    // Sender and the one domain it writes into. The top-level id stays, so IS-05 constrains mxl_flow_id
    // to the flow the writer writes and "auto" resolves to it; the domain tag
    // does the same for mxl_domain_id.
    inline std::string transportFile(Flow const& flow, std::string const& groupHint, std::string const& label, std::string const& domain)
    {
        auto definition = flow.definition;
        auto strings = [](std::string const& s) { return picojson::value{picojson::array{picojson::value{s}}}; };
        auto tags = picojson::object{};
        tags["urn:x-nmos:tag:grouphint/v1.0"] = strings(groupHint + ":" + role(flow));
        tags["urn:x-nvnmos:tag:name"] = strings(flow.name);
        tags["urn:x-nvnmos:tag:mxl-domain-id"] = strings(domain);
        definition["tags"] = picojson::value{tags};
        definition["label"] = picojson::value{label + " " + flow.name};
        if (textField(definition, "description").empty())
        {
            definition["description"] = picojson::value{label + " " + flow.name};
        }
        // An IS-04 rational without a denominator means 1, which the writer
        // reads it as too; stated, nothing downstream has to know that.
        for (auto const* key : {"grain_rate", "sample_rate"})
        {
            auto it = definition.find(key);
            if (it != definition.end() && it->second.is<picojson::object>())
            {
                auto& rational = it->second.get<picojson::object>();
                if (rational.find("denominator") == rational.end())
                {
                    rational["denominator"] = picojson::value{1.0};
                }
            }
        }
        return picojson::value{definition}.serialize();
    }
}
