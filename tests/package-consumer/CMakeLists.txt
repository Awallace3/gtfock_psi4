cmake_minimum_required(VERSION 3.24)
project(gtfock_package_consumer LANGUAGES C)

find_package(GTFock 0.1 CONFIG REQUIRED)

add_executable(gtfock_consumer main.c)
target_link_libraries(gtfock_consumer PRIVATE GTFock::GTFock)

enable_testing()
add_test(NAME consumer_runtime COMMAND gtfock_consumer)
