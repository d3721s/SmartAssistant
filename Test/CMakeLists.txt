add_executable(test_audio_manager
    test_audio_manager.cpp
)

target_link_libraries(test_audio_manager
    PRIVATE
        SmartAssistantAudioManager
)

add_test(
    NAME    test_audio_manager
    COMMAND test_audio_manager
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_executable(test_network_manager
    test_network_manager.cpp
)

target_link_libraries(test_network_manager
    PRIVATE
        SmartAssistantNetworkManager
)

add_test(
    NAME    test_network_manager
    COMMAND test_network_manager
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
