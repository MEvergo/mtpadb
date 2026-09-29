if(BUILD_TESTING)
    add_executable(mtprpc_protocol_test
        ${PROJECT_SOURCE_DIR}/tests/protocol/mtprpc_protocol_test.cpp
    )
    target_include_directories(mtprpc_protocol_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_features(mtprpc_protocol_test PRIVATE cxx_std_17)
    add_test(NAME mtprpc_protocol_test COMMAND mtprpc_protocol_test)
endif()
