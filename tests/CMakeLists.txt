# Unit tests reach the internal functions, so they link a static copy of the
# library (the shared one only exports the public API).
list(TRANSFORM RAG_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/ OUTPUT_VARIABLE RAG_TEST_SOURCES)
add_library(rag_internal STATIC ${RAG_TEST_SOURCES})
target_include_directories(rag_internal PUBLIC ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rag_internal PUBLIC ollama::ollama)
target_compile_options(rag_internal PRIVATE ${RAG_WARNINGS})
set_target_properties(rag_internal PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

foreach(name IN ITEMS chunk)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} PRIVATE rag_internal)
    target_compile_options(test_${name} PRIVATE ${RAG_WARNINGS})
    set_target_properties(test_${name} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
