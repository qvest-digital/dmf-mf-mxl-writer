# Built inside the qvest-digital/mxl tree as tools/mxl-writer-nmos, beside
# mxl-gst-testsrc, which it runs, and against the same toolchain.

include(GNUInstallDirs)

if (NOT TARGET picojson::picojson)
    find_package(picojson REQUIRED)
endif ()
# The tree's picojson::picojson carries only the directory of its wrapper
# header, not that of picojson.h the wrapper includes; targets linking mxl get
# that one through libmxl, and neither of these links it.
set(PICOJSON_HEADERS ${picojson_INCLUDE_DIR})

# NvNmos, NVIDIA's C API over nmos-cpp: include/nvnmos.h and lib/libnvnmos.so
# under NVNMOS_ROOT.
set(NVNMOS_ROOT "/opt/nvnmos" CACHE PATH "Where NvNmos is installed")
find_path(NVNMOS_INCLUDE_DIR nvnmos.h HINTS ${NVNMOS_ROOT}/include REQUIRED)
find_library(NVNMOS_LIBRARY nvnmos HINTS ${NVNMOS_ROOT}/lib REQUIRED)

add_executable(mxl-writer-nmos)
target_compile_features(mxl-writer-nmos PRIVATE cxx_std_20)
target_sources(mxl-writer-nmos PRIVATE mxl-writer-nmos.cpp)
target_include_directories(mxl-writer-nmos PRIVATE ${NVNMOS_INCLUDE_DIR} ${PICOJSON_HEADERS})
target_link_libraries(mxl-writer-nmos
        PRIVATE
            picojson::picojson
            ${NVNMOS_LIBRARY}
    )

add_executable(mxl-writer-nmos-tests)
target_compile_features(mxl-writer-nmos-tests PRIVATE cxx_std_20)
target_sources(mxl-writer-nmos-tests PRIVATE senders-test.cpp)
target_include_directories(mxl-writer-nmos-tests PRIVATE ${PICOJSON_HEADERS})
target_link_libraries(mxl-writer-nmos-tests PRIVATE picojson::picojson)

# The conformance lane's reader of the flows, test/nmos/phase-probe.cpp,
# where the image build copies it in. Built in the build stage only.
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/phase-probe.cpp)
    add_executable(mxl-phase-probe)
    target_compile_features(mxl-phase-probe PRIVATE cxx_std_20)
    target_sources(mxl-phase-probe PRIVATE phase-probe.cpp)
    target_link_libraries(mxl-phase-probe PRIVATE mxl)
endif ()
