find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

add_executable(
    sys_prog_allctr_allctr_bdds_sstm_tests
    allocator_buddies_system_tests.cpp
)

target_link_libraries(
    sys_prog_allctr_allctr_bdds_sstm_tests
    PRIVATE
    GTest::gtest_main
    Threads::Threads
    sys_prog_allctr_allctr_bdds_sstm
)

target_compile_features(
    sys_prog_allctr_allctr_bdds_sstm_tests
    PRIVATE
    cxx_std_20
)

include(GoogleTest)
gtest_discover_tests(sys_prog_allctr_allctr_bdds_sstm_tests)