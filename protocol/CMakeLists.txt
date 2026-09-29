add_library(mtprpc_protocol STATIC src/mtprpc_protocol.cpp)
target_include_directories(mtprpc_protocol
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(mtprpc_protocol PUBLIC cxx_std_17)

if(BUILD_TESTING)
    add_executable(mtprpc_protocol_test
        ${PROJECT_SOURCE_DIR}/tests/protocol/mtprpc_protocol_test.cpp
    )
    target_include_directories(mtprpc_protocol_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_features(mtprpc_protocol_test PRIVATE cxx_std_17)
    target_link_libraries(mtprpc_protocol_test PRIVATE mtprpc_protocol)
    add_test(NAME mtprpc_protocol_test COMMAND mtprpc_protocol_test)
    add_executable(mtprpc_crypto_test
        ${PROJECT_SOURCE_DIR}/tests/protocol/mtprpc_crypto_test.cpp
    )
    target_include_directories(mtprpc_crypto_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_features(mtprpc_crypto_test PRIVATE cxx_std_17)
    target_link_libraries(mtprpc_crypto_test PRIVATE mtprpc_protocol)
    add_test(NAME mtprpc_crypto_test COMMAND mtprpc_crypto_test)
endif()
