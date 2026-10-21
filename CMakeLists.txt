cmake_minimum_required(VERSION 3.14)

project(ollama-rag-c
    VERSION 0.1.0
    DESCRIPTION "Local Retrieval-Augmented Generation in C, with Ollama and SQLite"
    LANGUAGES C)

include(GNUInstallDirs)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(RAG_TOP_LEVEL ON)
else()
    set(RAG_TOP_LEVEL OFF)
endif()

option(BUILD_SHARED_LIBS "Build ollama-rag as a shared library" OFF)
option(RAG_BUILD_EXAMPLES "Build the examples" ${RAG_TOP_LEVEL})
option(RAG_BUILD_TESTS "Build the tests" ${RAG_TOP_LEVEL})

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(RAG_WARNINGS -Wall -Wextra -pedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla)
elseif(MSVC)
    set(RAG_WARNINGS /W4)
endif()

# ---- dependencies ----

include(FetchContent)
FetchContent_Declare(ollama_c
    GIT_REPOSITORY https://github.com/Topogamer0/Ollama-C.git
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ollama_c)

# ---- library ----

set(RAG_SOURCES
    src/chunk.c
    src/utf8.c
    src/util.c)

add_library(ollama_rag ${RAG_SOURCES})
add_library(ollama::rag ALIAS ollama_rag)
target_include_directories(ollama_rag PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(ollama_rag PRIVATE ollama::ollama)
target_compile_options(ollama_rag PRIVATE ${RAG_WARNINGS})
set_target_properties(ollama_rag PROPERTIES
    OUTPUT_NAME ollama-rag
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

if(RAG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
