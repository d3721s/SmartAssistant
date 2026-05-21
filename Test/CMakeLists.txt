add_executable(test_audio_manager
    test_audio_manager.cpp
)

target_link_libraries(test_audio_manager
    PRIVATE
        SmartAssistantAudioManager
)

add_test(NAME test_audio_manager COMMAND test_audio_manager)
