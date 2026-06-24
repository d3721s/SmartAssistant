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

if(TARGET SmartAssistantWakeupManager)
    add_executable(test_wakeup_manager
        test_wakeup_manager.cpp
    )

    target_link_libraries(test_wakeup_manager
        PRIVATE
            SmartAssistantWakeupManager
    )

    add_test(
        NAME    test_wakeup_manager
        COMMAND test_wakeup_manager
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    add_executable(wakeup_pulse_listener
        wakeup_pulse_listener.cpp
    )

    target_link_libraries(wakeup_pulse_listener
        PRIVATE
            SmartAssistantWakeupManager
    )
endif()

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

if(TARGET SmartAssistantTTSManager)
    add_executable(test_tts_manager
        test_tts_manager.cpp
    )

    target_link_libraries(test_tts_manager
        PRIVATE
            SmartAssistantTTSManager
            SmartAssistantAudioManager
    )
endif()

if(TARGET SmartAssistantE2EChat)
    add_executable(test_chat
        test_chat.cpp
    )

    target_link_libraries(test_chat
        PRIVATE
            SmartAssistantE2EChat
            SmartAssistantAudioManager
    )
endif()

if(TARGET SmartAssistantOrchestrator AND
   TARGET SmartAssistantWakeupManager AND TARGET SmartAssistantE2EChat AND
   TARGET SmartAssistantTTSManager AND TARGET SmartAssistantIntentManager AND
   TARGET SmartAssistantWheelchairController AND
   TARGET SmartAssistantArmController AND
   TARGET SmartAssistantGripperController AND
   TARGET SmartAssistantMusicPlayer AND
   TARGET SmartAssistantNavigationController AND
   TARGET SmartAssistantSystemVolumeController)
    add_executable(test_voice_assistant
        test_voice_assistant.cpp
    )

    target_link_libraries(test_voice_assistant
        PRIVATE
            SmartAssistantOrchestrator
            SmartAssistantWakeupManager
            SmartAssistantE2EChat
            SmartAssistantTTSManager
            SmartAssistantIntentManager
            SmartAssistantWheelchairController
            SmartAssistantArmController
            SmartAssistantGripperController
            SmartAssistantMusicPlayer
            SmartAssistantNavigationController
            SmartAssistantSystemVolumeController
            SmartAssistantAudioManager
            tomlplusplus::tomlplusplus
    )
endif()

if(TARGET SmartAssistantASRManager)
    add_executable(test_asr_manager
        test_asr_manager.cpp
    )

    target_link_libraries(test_asr_manager
        PRIVATE
            SmartAssistantASRManager
            SmartAssistantAudioManager
    )

    add_test(
        NAME    test_asr_manager
        COMMAND test_asr_manager
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()

if(TARGET SmartAssistantIntentManager)
    add_executable(test_intent_manager
        test_intent_manager.cpp
    )

    target_link_libraries(test_intent_manager
        PRIVATE
            SmartAssistantIntentManager
    )

    add_test(
        NAME    test_intent_manager
        COMMAND test_intent_manager
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()

if(TARGET SmartAssistantWheelchairController)
    add_executable(test_wheelchair_controller
        test_wheelchair_controller.cpp
    )

    target_link_libraries(test_wheelchair_controller
        PRIVATE
            SmartAssistantWheelchairController
    )

    # Not registered with add_test by default: this driver actually drives a
    # ROS2 publisher (its own rclcpp::Node), so it is meant to be run in a
    # sourced ROS2 shell where someone can `ros2 topic echo /cmd_vel` on the
    # side.  Invoke manually:  ./build/Test/test_wheelchair_controller
endif()
