find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

add_executable(
    sys_prog_allctr_allctr_glbl_hp_tests
    allocator_global_heap_tests.cpp
)

target_link_libraries(
    sys_prog_allctr_allctr_glbl_hp_tests
    PRIVATE
    GTest::gtest_main
    Threads::Threads
    sys_prog_allctr_allctr_glbl_hp
)

include(GoogleTest)
gtest_discover_tests(sys_prog_allctr_allctr_glbl_hp_tests)