# Built inside the qvest-digital/mxl tree as tools/mxl-gst-flowdef-test, so it
# compiles mxl-gst-testsrc's own json_utils against the same picojson and libmxl
# headers.
add_executable(mxl-gst-flowdef-test)
target_compile_features(mxl-gst-flowdef-test PRIVATE cxx_std_20)
target_sources(mxl-gst-flowdef-test PRIVATE flowdef-test.cpp)
if (NOT TARGET fmt::fmt)
    find_package(fmt CONFIG REQUIRED)
endif ()
if (NOT TARGET picojson::picojson)
    find_package(picojson REQUIRED)
endif ()
target_link_libraries(mxl-gst-flowdef-test
    PRIVATE
        mxl
        fmt::fmt
        picojson::picojson
)
