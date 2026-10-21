cmake_minimum_required(VERSION 3.14)
# MSVC: no /W3 in the default flags, which /W4 would override with a warning per file
if(POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW)
endif()
# the behaviors that newer versions of CMake ask about: normalized install destinations and paths of DLLs
foreach(policy CMP0177 CMP0207)
    if(POLICY ${policy})
        cmake_policy(SET ${policy} NEW)
    endif()
endforeach()

project(ollama-rag-c
    VERSION 0.2.0
    DESCRIPTION "Local Retrieval-Augmented Generation in C, with Ollama and SQLite"
    LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(RAG_TOP_LEVEL ON)
else()
    set(RAG_TOP_LEVEL OFF)
endif()

option(BUILD_SHARED_LIBS "Build ollama-rag as a shared library" OFF)
option(RAG_BUILD_CLI "Build the ragc command line tool" ON)
option(RAG_BUILD_EXAMPLES "Build the examples" ${RAG_TOP_LEVEL})
option(RAG_BUILD_TESTS "Build the tests" ${RAG_TOP_LEVEL})
option(RAG_INSTALL "Generate the install target" ${RAG_TOP_LEVEL})
option(RAG_USE_SYSTEM_SQLITE "Link the system SQLite instead of the bundled amalgamation" OFF)
option(RAG_USE_SYSTEM_OLLAMA_C "Use an installed ollama-c instead of downloading it" OFF)
option(RAG_WERROR "Treat the warnings of ollama-rag-c (not of its dependencies) as errors" OFF)
option(RAG_INSTALL_RUNTIME_DLLS "Windows: install the DLLs that ragc needs (libcurl, the C runtime) next to it" OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(RAG_WARNINGS -Wall -Wextra -pedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla)
    if(RAG_WERROR)
        list(APPEND RAG_WARNINGS -Werror)
    endif()
elseif(MSVC)
    # C4204, C4221: aggregate initializers that C99 allows, and the default dialect of MSVC calls extensions
    set(RAG_WARNINGS /W4 /wd4204 /wd4221)
    if(RAG_WERROR)
        list(APPEND RAG_WARNINGS /WX)
    endif()
endif()

# On Windows, programs find their DLLs in their own directory: put them all together.
if(WIN32 AND RAG_TOP_LEVEL AND NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()

# ---- dependencies ----

if(RAG_USE_SYSTEM_OLLAMA_C)
    find_package(ollama-c 0.1 REQUIRED)
else()
    # installed along with ollama-rag-c, which needs it
    set(OLLAMA_INSTALL ${RAG_INSTALL})
    set(OLLAMA_BUILD_TESTS OFF)
    set(OLLAMA_BUILD_EXAMPLES OFF)
    include(FetchContent)
    FetchContent_Declare(ollama_c
        GIT_REPOSITORY https://github.com/Topogamer0/Ollama-C.git
        GIT_TAG v0.1.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(ollama_c)
endif()

# after Ollama-C, so that it keeps its own documentation directory
include(GNUInstallDirs)

if(MSVC)
    # for our sources only: they are UTF-8, and they use the standard C functions (fopen, strerror, ...)
    add_compile_options(/utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

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
        target_link_libraries(${target} PRIVATE m)
    endif()
endfunction()

# ---- library ----

set(RAG_SOURCES
    src/ask.c
    src/chunk.c
    src/compat.c
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
if(BUILD_SHARED_LIBS)
    # the functions marked RAG_API are exported from the DLL, and imported by its users
    target_compile_definitions(ollama_rag PRIVATE RAG_BUILDING_SHARED INTERFACE RAG_SHARED)
endif()
set_target_properties(ollama_rag PROPERTIES
    OUTPUT_NAME ollama-rag
    EXPORT_NAME rag
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

if(RAG_BUILD_CLI)
    # ragc uses the portability layer of the library too: a copy of its own, since a DLL does not export it
    add_executable(ragc cli/ragc.c src/compat.c)
    target_include_directories(ragc PRIVATE src)
    target_link_libraries(ragc PRIVATE ollama::rag)
    target_compile_options(ragc PRIVATE ${RAG_WARNINGS})
    set_target_properties(ragc PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MINGW)
        target_link_options(ragc PRIVATE -municode) # wmain()
    endif()
endif()

# installed shared libraries find each other, and ragc finds them
if(BUILD_SHARED_LIBS)
    if(APPLE)
        set(RAG_ORIGIN "@loader_path")
    else()
        set(RAG_ORIGIN "$ORIGIN")
    endif()
    set_target_properties(ollama_rag PROPERTIES INSTALL_RPATH "${RAG_ORIGIN}")
    if(RAG_BUILD_CLI)
        set_target_properties(ragc PROPERTIES INSTALL_RPATH "${RAG_ORIGIN}/../${CMAKE_INSTALL_LIBDIR}")
    endif()
endif()

if(RAG_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(RAG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- installation ----

if(RAG_INSTALL)
    include(CMakePackageConfigHelpers)
    set(RAG_DOCDIR ${CMAKE_INSTALL_DATAROOTDIR}/doc/${PROJECT_NAME})

    install(TARGETS ollama_rag EXPORT ollama-rag-c-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(RAG_BUILD_CLI AND WIN32 AND RAG_INSTALL_RUNTIME_DLLS)
        # a ragc.exe that runs on its own: the DLLs it loads (libcurl, zlib, ...), but the system's
        if(CMAKE_VERSION VERSION_LESS 3.21 OR CMAKE_CROSSCOMPILING)
            message(FATAL_ERROR "RAG_INSTALL_RUNTIME_DLLS needs CMake 3.21 or later, on Windows")
        endif()
        find_package(CURL REQUIRED) # where its DLLs are
        install(TARGETS ragc RUNTIME_DEPENDENCY_SET rag_runtime RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        if(BUILD_SHARED_LIBS)
            # the DLLs of the project are not searched for the DLLs they load (libcurl) unless they are in the set
            set(RAG_DLL_TARGETS ollama_rag)
            if(NOT RAG_USE_SYSTEM_OLLAMA_C)
                list(APPEND RAG_DLL_TARGETS ollama)
            endif()
            install(TARGETS ${RAG_DLL_TARGETS}
                RUNTIME_DEPENDENCY_SET rag_runtime
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
        endif()
        install(RUNTIME_DEPENDENCY_SET rag_runtime
            DIRECTORIES $<TARGET_FILE_DIR:ragc> $<TARGET_FILE_DIR:CURL::libcurl>
            PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
            POST_EXCLUDE_REGEXES "[Ss][Yy][Ss][Tt][Ee][Mm]32"
            DESTINATION ${CMAKE_INSTALL_BINDIR})
        if(MSVC)
            # and the C runtime of the compiler, which Windows does not always have
            set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
            include(InstallRequiredSystemLibraries)
            list(FILTER CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS INCLUDE REGEX "vcruntime[^/]*\\.dll$")
            if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
                message(FATAL_ERROR "RAG_INSTALL_RUNTIME_DLLS: the C runtime (vcruntime140.dll) was not found")
            endif()
            install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION ${CMAKE_INSTALL_BINDIR})
        endif()
    elseif(RAG_BUILD_CLI)
        install(TARGETS ragc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    install(FILES include/ollama_rag.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES LICENSE README.md DESTINATION ${RAG_DOCDIR})
    install(EXPORT ollama-rag-c-targets
        NAMESPACE ollama::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ollama-rag-c)

    if(RAG_USE_SYSTEM_SQLITE)
        set(RAG_FIND_SQLITE "find_dependency(SQLite3)\n")
        set(RAG_PC_SQLITE ", sqlite3")
    else()
        set(RAG_FIND_SQLITE "find_dependency(Threads)\n")
        set(RAG_PC_SQLITE "")
    endif()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ollama-rag-c-config.cmake
        "include(CMakeFindDependencyMacro)\n"
        "find_dependency(ollama-c 0.1)\n"
        "${RAG_FIND_SQLITE}"
        "include(\"\${CMAKE_CURRENT_LIST_DIR}/ollama-rag-c-targets.cmake\")\n")
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ollama-rag-c-config-version.cmake
        COMPATIBILITY SameMinorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ollama-rag-c-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ollama-rag-c-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ollama-rag-c)

    # relative to the .pc file, so that the installation can be moved
    if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
        set(RAG_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
    else()
        file(RELATIVE_PATH RAG_PC_RELATIVE "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/pkgconfig"
            "${CMAKE_INSTALL_PREFIX}") # full paths, also on Windows
        set(RAG_PC_PREFIX "\${pcfiledir}/${RAG_PC_RELATIVE}")
    endif()
    # a static library needs its dependencies on the link line of its users
    set(RAG_PC_LIBS "")
    if(NOT RAG_USE_SYSTEM_SQLITE AND CMAKE_THREAD_LIBS_INIT)
        string(APPEND RAG_PC_LIBS " ${CMAKE_THREAD_LIBS_INIT}")
    endif()
    if(RAG_MATH_LIBRARY)
        string(APPEND RAG_PC_LIBS " -lm")
    endif()
    if(BUILD_SHARED_LIBS)
        set(RAG_PC_REQUIRES "Requires.private")
        set(RAG_PC_LIBS_PRIVATE "${RAG_PC_LIBS}")
        set(RAG_PC_LIBS "")
    else()
        set(RAG_PC_REQUIRES "Requires")
        set(RAG_PC_LIBS_PRIVATE "")
    endif()
    configure_file(ollama-rag-c.pc.in ${CMAKE_CURRENT_BINARY_DIR}/ollama-rag-c.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ollama-rag-c.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()
