add_library(mtpadb_functionfs_descriptors gadget/functionfs_descriptors.cpp)
target_include_directories(mtpadb_functionfs_descriptors
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gadget
)
target_compile_features(mtpadb_functionfs_descriptors PUBLIC cxx_std_20)
add_library(mtpadb_gadget_config gadget/gadget_config.cpp)
target_include_directories(mtpadb_gadget_config
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gadget
)
target_compile_features(mtpadb_gadget_config PUBLIC cxx_std_20)


add_executable(mtpadb-gadgetd
    gadget/main.cpp
    gadget/gadget_manager.cpp
)
target_link_libraries(mtpadb-gadgetd PRIVATE
    mtpadb_functionfs_descriptors
    mtpadb_gadget_config
)
target_compile_features(mtpadb-gadgetd PRIVATE cxx_std_20)
configure_file(
    ${PROJECT_SOURCE_DIR}/linux/systemd/mtpadb-gadgetd.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/mtpadb-gadgetd.service
    @ONLY
)
install(TARGETS mtpadb-gadgetd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(FILES
    ${PROJECT_SOURCE_DIR}/linux/host.conf.example
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/mtpadb
)
install(PROGRAMS
    ${PROJECT_SOURCE_DIR}/linux/scripts/mtpadb-gadget-reset
    DESTINATION ${CMAKE_INSTALL_SBINDIR}
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/mtpadb-gadgetd.service
    DESTINATION lib/systemd/system
)

if(BUILD_TESTING)
    add_executable(functionfs_descriptors_test
        ${PROJECT_SOURCE_DIR}/tests/gadget/functionfs_descriptors_test.cpp
    )
    target_include_directories(functionfs_descriptors_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gadget
    )
    target_compile_features(functionfs_descriptors_test PRIVATE cxx_std_20)
    target_link_libraries(functionfs_descriptors_test PRIVATE mtpadb_functionfs_descriptors)
    add_test(NAME functionfs_descriptors COMMAND functionfs_descriptors_test)
    add_executable(gadget_config_test
        ${PROJECT_SOURCE_DIR}/tests/gadget/gadget_config_test.cpp
    )
    target_include_directories(gadget_config_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gadget
    )
    target_compile_features(gadget_config_test PRIVATE cxx_std_20)
    target_link_libraries(gadget_config_test PRIVATE mtpadb_gadget_config)
    add_test(NAME gadget_config COMMAND gadget_config_test)
endif()
