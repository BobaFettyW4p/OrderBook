cmake_minimum_required(VERSION 3.5)
project(OrderBook)

set(CMAKE_CXX_STANDARD 20)

# Find threading package
find_package(Threads REQUIRED)

add_executable(OrderBook orderbook.cpp)

# Link with the threading library
target_link_libraries(OrderBook PRIVATE Threads::Threads)