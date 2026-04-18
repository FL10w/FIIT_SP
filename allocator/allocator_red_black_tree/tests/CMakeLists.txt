# Библиотека аллокатора
add_library(
    sys_prog_allctr_allctr_rb_tr
    ../src/allocator_red_black_tree.cpp
    ../src/pp_allocator.cpp
)

target_include_directories(
    sys_prog_allctr_allctr_rb_tr
    PUBLIC
    ../include
    ../../allocator/include
    ../../common/include
)

target_compile_features(
    sys_prog_allctr_allctr_rb_tr
    PRIVATE
    cxx_std_20
)

# Тесты
add_executable(
    sys_prog_allctr_allctr_rb_tr_tests
    allocator_red_black_tree_tests.cpp
)

target_link_libraries(
    sys_prog_allctr_allctr_rb_tr_tests
    PRIVATE
    gtest
    gtest_main
    sys_prog_allctr_allctr_rb_tr
)

target_compile_features(
    sys_prog_allctr_allctr_rb_tr_tests
    PRIVATE
    cxx_std_20
)