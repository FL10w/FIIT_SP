add_subdirectory(tests)

find_package(Boost COMPONENTS system container REQUIRED)

add_library(
        sys_prog_assctv_cntnr_indxng_tr_b_tr
        include/b_tree.h
        src/hhh.cpp)

target_include_directories(
        sys_prog_assctv_cntnr_indxng_tr_b_tr
        PUBLIC
        ./include
        ${Boost_INCLUDE_DIRS})

target_link_directories(
        sys_prog_assctv_cntnr_indxng_tr_b_tr
        PUBLIC
        ${Boost_LIBRARY_DIRS})

target_link_libraries(
        sys_prog_assctv_cntnr_indxng_tr_b_tr
        PUBLIC
        sys_prog_assctv_cntnr
        ${Boost_LIBRARIES})