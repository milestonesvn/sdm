add_subdirectory(gtest EXCLUDE_FROM_ALL)
include_directories(gtest/include)

add_executable(sdm_tests EXCLUDE_FROM_ALL tests.cpp)
target_link_libraries(sdm_tests
    sdm
    gtest
)

add_executable(test_c_interface test_c_interface.c)
target_link_libraries(test_c_interface
    sdm
    flann
)

add_custom_target(runtests
    COMMAND sdm_tests
    DEPENDS sdm_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
