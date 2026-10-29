# Built inside the qvest-digital/mxl tree as tools/mxl-gst-faults-test, beside
# mxl-gst-testsrc, whose faults.hpp it tests.
add_executable(mxl-gst-faults-test)
target_compile_features(mxl-gst-faults-test PRIVATE cxx_std_20)
target_sources(mxl-gst-faults-test PRIVATE faults-test.cpp)
find_package(Threads REQUIRED)
target_link_libraries(mxl-gst-faults-test PRIVATE Threads::Threads)
