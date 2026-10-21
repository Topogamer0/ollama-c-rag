# Builds a program against an installed ollama-rag-c, to check the installation:
#   cmake -S tests/install -B build-consumer -DCMAKE_PREFIX_PATH=<prefix>
cmake_minimum_required(VERSION 3.14)
project(ollama-rag-c-consumer LANGUAGES C)

find_package(ollama-rag-c 0.2 REQUIRED)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE ollama::rag)
