add_library(mtpadb_rpc_client STATIC rpc_client.cpp)
target_include_directories(mtpadb_rpc_client
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(mtpadb_rpc_client PUBLIC cxx_std_17)
target_link_libraries(mtpadb_rpc_client PUBLIC mtprpc_protocol)

if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(mtpadb_rpc_client_test
        ${PROJECT_SOURCE_DIR}/tests/adb/mtpadb_rpc_client_test.cpp
        ${PROJECT_SOURCE_DIR}/android/mtprpcd/service.cpp
    )
    target_include_directories(mtpadb_rpc_client_test
        PRIVATE ${PROJECT_SOURCE_DIR}/android
    )
    target_compile_features(mtpadb_rpc_client_test PRIVATE cxx_std_17)
    target_link_libraries(mtpadb_rpc_client_test PRIVATE
        mtpadb_rpc_client
        mtprpc_protocol
        Threads::Threads
    )
    add_test(NAME mtpadb_rpc_client_test COMMAND mtpadb_rpc_client_test)
    set_tests_properties(mtpadb_rpc_client_test PROPERTIES TIMEOUT 30)
endif()
