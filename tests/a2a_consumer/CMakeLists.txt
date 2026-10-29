cmake_minimum_required(VERSION 3.21)

project(a2a_consumer LANGUAGES CXX)

# 这个工程模拟“外部消费者”：只用安装后的 a2a 包，不直接引用源码树。
find_package(a2a CONFIG REQUIRED)

add_executable(a2a_consumer main.cpp)
target_compile_features(a2a_consumer PRIVATE cxx_std_20)
target_link_libraries(a2a_consumer PRIVATE a2a::a2a)
