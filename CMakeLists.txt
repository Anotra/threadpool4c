cmake_minimum_required(VERSION 3.10)


project(threadpool4c)
find_package(Threads REQUIRED)
add_library(${PROJECT_NAME} SHARED
  src/threadpool.c
)
target_include_directories(${PROJECT_NAME} PUBLIC include)
target_link_libraries(${PROJECT_NAME} Threads::Threads)
set_property(TARGET ${PROJECT_NAME} PROPERTY C_STANDARD 99)


project(test_threadpool)
add_executable(${PROJECT_NAME}
  test/test.c
)
target_link_libraries(${PROJECT_NAME} PRIVATE threadpool4c)
