find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(mtprpc_protocol STATIC
    src/mtprpc_protocol.cpp
    src/mtprpc_crypto.cpp
    src/mtprpc_session.cpp
)
target_include_directories(mtprpc_protocol
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(mtprpc_protocol PUBLIC cxx_std_17)
target_link_libraries(mtprpc_protocol PUBLIC OpenSSL::Crypto)

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
    find_package(Threads REQUIRED)
    add_executable(mtprpcd_socket_test
        ${PROJECT_SOURCE_DIR}/tests/adb/mtprpcd_socket_test.cpp
        ${PROJECT_SOURCE_DIR}/android/mtprpcd/service.cpp
    )
    target_include_directories(mtprpcd_socket_test
        PRIVATE ${PROJECT_SOURCE_DIR}/android
    )
    target_compile_features(mtprpcd_socket_test PRIVATE cxx_std_17)
    target_link_libraries(mtprpcd_socket_test PRIVATE mtprpc_protocol Threads::Threads)
    add_test(NAME mtprpcd_socket_test COMMAND mtprpcd_socket_test)
    set_tests_properties(mtprpcd_socket_test PROPERTIES TIMEOUT 30)
endif()
