# Built inside the qvest-digital/mxl tree as tools/mxl-writer-status, so it
# links the same libmxl target and resolves its dependencies from the same
# toolchain as mxl-gst-testsrc.

include(GNUInstallDirs)

add_executable(mxl-writer-status)
target_compile_features(mxl-writer-status PRIVATE cxx_std_20)
target_sources(mxl-writer-status PRIVATE mxl-writer-status.cpp)

# The release version, or the commit, the image is built from; the binary
# prints it at start, on --version and on /status.
set(MXL_WRITER_STATUS_VERSION "dev" CACHE STRING "Version mxl-writer-status reports")
target_compile_definitions(mxl-writer-status PRIVATE MXL_WRITER_STATUS_VERSION="${MXL_WRITER_STATUS_VERSION}")

if (NOT TARGET CLI11::CLI11)
    find_package(CLI11 CONFIG REQUIRED)
endif ()

if (NOT TARGET fmt::fmt)
    find_package(fmt CONFIG REQUIRED)
endif ()

if (NOT TARGET picojson::picojson)
    find_package(picojson REQUIRED)
endif ()

target_link_libraries(mxl-writer-status
        PRIVATE
            mxl
            CLI11::CLI11
            fmt::fmt
            picojson::picojson
    )

set_target_properties(mxl-writer-status
        PROPERTIES
            INSTALL_RPATH "${MXL_TOOLS_INSTALL_RPATH}"
    )

add_executable(mxl-writer-status-tests)
target_compile_features(mxl-writer-status-tests PRIVATE cxx_std_20)
target_sources(mxl-writer-status-tests PRIVATE status-test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(mxl-writer-status-tests
        PRIVATE
            mxl
            fmt::fmt
            picojson::picojson
            Threads::Threads
    )
