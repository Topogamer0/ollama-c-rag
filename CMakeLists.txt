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
option(RAG_BUILD_CLI "Build the ragc command line tool" ON)
option(RAG_BUILD_EXAMPLES "Build the examples" ${RAG_TOP_LEVEL})
option(RAG_BUILD_TESTS "Build the tests" ${RAG_TOP_LEVEL})
option(RAG_USE_SYSTEM_SQLITE "Link the system SQLite instead of the bundled amalgamation" OFF)

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

if(RAG_USE_SYSTEM_SQLITE)
    find_package(SQLite3 REQUIRED)
else()
    # The amalgamation is compiled with the library, without warnings: it is not our code.
    find_package(Threads REQUIRED)
    add_library(rag_sqlite OBJECT third_party/sqlite/sqlite3.c)
    target_compile_definitions(rag_sqlite PRIVATE
        SQLITE_THREADSAFE=1
        SQLITE_DQS=0
        SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_OMIT_DEPRECATED)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(rag_sqlite PRIVATE -w)
    elseif(MSVC)
        target_compile_options(rag_sqlite PRIVATE /w)
    endif()
    set_target_properties(rag_sqlite PROPERTIES
        C_STANDARD 99
        C_EXTENSIONS ON
        C_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON)
endif()

find_library(RAG_MATH_LIBRARY m)

# Adds SQLite and the system libraries to a target built from RAG_SOURCES.
function(rag_link_dependencies target)
    target_link_libraries(${target} PRIVATE ollama::ollama)
    if(RAG_USE_SYSTEM_SQLITE)
        target_link_libraries(${target} PRIVATE SQLite::SQLite3)
    else()
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:rag_sqlite>)
        target_include_directories(${target} SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/sqlite)
        target_link_libraries(${target} PRIVATE Threads::Threads)
    endif()
    if(RAG_MATH_LIBRARY)
        target_link_libraries(${target} PRIVATE ${RAG_MATH_LIBRARY})
    endif()
endfunction()

# ---- library ----

set(RAG_SOURCES
    src/ask.c
    src/chunk.c
    src/db.c
    src/embed.c
    src/fs.c
    src/index.c
    src/rag.c
    src/search.c
    src/sha256.c
    src/utf8.c
    src/util.c
    src/vector.c)

add_library(ollama_rag ${RAG_SOURCES})
add_library(ollama::rag ALIAS ollama_rag)
target_include_directories(ollama_rag PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
rag_link_dependencies(ollama_rag)
target_compile_options(ollama_rag PRIVATE ${RAG_WARNINGS})
set_target_properties(ollama_rag PROPERTIES
    OUTPUT_NAME ollama-rag
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

if(RAG_BUILD_CLI)
    add_executable(ragc cli/ragc.c)
    target_link_libraries(ragc PRIVATE ollama::rag)
    target_compile_options(ragc PRIVATE ${RAG_WARNINGS})
    set_target_properties(ragc PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endif()

if(RAG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
