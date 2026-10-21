# Unit tests reach the internal functions, so they link a static copy of the
# library (the shared one only exports the public API).
list(TRANSFORM RAG_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/ OUTPUT_VARIABLE RAG_TEST_SOURCES)
add_library(rag_internal STATIC ${RAG_TEST_SOURCES})
target_include_directories(rag_internal PUBLIC ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
rag_link_dependencies(rag_internal)
target_link_libraries(rag_internal PUBLIC ollama::ollama)
# the database tests create databases of their own
if(RAG_USE_SYSTEM_SQLITE)
    target_link_libraries(rag_internal PUBLIC SQLite::SQLite3)
else()
    target_include_directories(rag_internal SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/sqlite)
endif()
target_compile_options(rag_internal PRIVATE ${RAG_WARNINGS})
set_target_properties(rag_internal PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

foreach(name IN ITEMS chunk db fs vector)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} PRIVATE rag_internal)
    target_compile_options(test_${name} PRIVATE ${RAG_WARNINGS})
    set_target_properties(test_${name} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME ${name} COMMAND test_${name})
endforeach()

# Integration tests use the public API against a fake Ollama server, and the
# portability layer for their temporary files (a DLL does not export it).
find_package(CURL REQUIRED)
add_executable(test_rag test_rag.c ${PROJECT_SOURCE_DIR}/src/compat.c)
target_include_directories(test_rag PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_rag PRIVATE ollama::rag CURL::libcurl)
target_compile_options(test_rag PRIVATE ${RAG_WARNINGS})
set_target_properties(test_rag PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME rag COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mock_server.py $<TARGET_FILE:test_rag>)
    if(RAG_BUILD_CLI)
        add_test(NAME cli
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mock_server.py
                    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py $<TARGET_FILE:ragc>)
    endif()
else()
    message(STATUS "Python 3 not found: the integration tests are disabled")
endif()
