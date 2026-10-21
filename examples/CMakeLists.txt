set(RAG_EXAMPLES
    quickstart
    index-notes
    chat-docs)

foreach(name IN LISTS RAG_EXAMPLES)
    add_executable(example-${name} ${name}.c)
    target_link_libraries(example-${name} PRIVATE ollama::rag)
    target_compile_options(example-${name} PRIVATE ${RAG_WARNINGS})
    set_target_properties(example-${name} PROPERTIES OUTPUT_NAME ${name} C_STANDARD 99 C_EXTENSIONS OFF)
    if(MSVC)
        target_sources(example-${name} PRIVATE utf8.manifest) # UTF-8 arguments
    endif()
endforeach()
