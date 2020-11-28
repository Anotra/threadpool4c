cmake_minimum_required(VERSION 3.10)


project(threadpool4c)
find_package(Threads REQUIRED)
add_library(threadpool4c SHARED
  src/threadpool.c
)
target_include_directories(threadpool4c PUBLIC include)
target_link_libraries(threadpool4c ${CMAKE_THREAD_LIBS_INIT})
set_property(TARGET threadpool4c PROPERTY C_STANDARD 99)


project(test)
add_executable(test
  test/test.c
)
target_link_libraries(test PRIVATE threadpool4c)