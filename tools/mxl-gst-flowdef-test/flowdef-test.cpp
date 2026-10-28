// Tests the flow definition mxl-gst-testsrc hands to libmxl, which stores it
// verbatim as flow_def.json. Built and run by the image build, so an image
// whose flow_def.json carries "\/" is never published.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "../mxl-gst/utils.hpp"

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

    // Whether text has the escape "\/", reading each escape whole: a literal
    // backslash before a '/' serializes as "\\/", which is not one.
    bool escapesSlash(std::string const& text)
    {
        for (auto i = std::size_t{0}; i + 1 < text.size(); ++i)
        {
            if (text[i] == '\\')
            {
                if (text[i + 1] == '/')
                {
                    return true;
                }
                ++i;
            }
        }
        return false;
    }

    // The definition the way mxl-gst-testsrc builds it: read from the config
    // file, group hint set, serialized.
    std::string definition(std::string const& file)
    {
        auto const path = std::filesystem::temp_directory_path() / "mxl-gst-flowdef-test.json";
        std::ofstream{path} << file;
        auto flow = json_utils::parseFile(path);
        std::filesystem::remove(path);
        json_utils::updateGroupHint(flow, "writer-1", "video");
        return json_utils::serializeJson(flow);
    }
}

int main()
{
    auto const def = definition(R"({
        "id": "11111111-2222-4333-8444-555555555555",
        "format": "urn:x-nmos:format:video",
        "media_type": "video/v210",
        "label": "a/b \\/ c \"q\" \u001f",
        "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["x:video"]}
    })");

    check(!escapesSlash(def), "no '/' is written as \"\\/\"");
    check(def.find(R"("media_type":"video/v210")") != std::string::npos, "media_type is \"video/v210\" as text");

    auto parsed = picojson::value{};
    check(picojson::parse(parsed, def).empty(), "the definition is still valid JSON");
    auto const& object = parsed.get<picojson::object>();
    check(object.at("label").get<std::string>() == "a/b \\/ c \"q\" \x1f", "every other escape reads back unchanged");
    check(object.at("tags").get<picojson::object>().at("urn:x-nmos:tag:grouphint/v1.0").get<picojson::array>()[0].get<std::string>() ==
              "writer-1:video",
        "the group hint is set");

    if (failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("mxl-gst-flowdef-test: ok");
    return 0;
}
