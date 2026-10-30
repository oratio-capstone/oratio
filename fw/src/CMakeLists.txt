if(ORATIO_AUDIO_OUTPUT_TEST AND ORATIO_AUDIO_PLAYBACK_TEST)
    message(FATAL_ERROR "Select only one audio diagnostic application")
endif()

set(app_includes ".")
if(ORATIO_AUDIO_PLAYBACK_TEST)
    set(fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/speech_fixture")
    set(fixture_generator "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_playback_fixture.py")
    set(fixture_sources "${CMAKE_CURRENT_LIST_DIR}/../test/test_162_audio_playback/sources")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${fixture_generator}" "${fixture_sources}/manifest.json"
        "${fixture_sources}/1_jackson_0.wav" "${fixture_sources}/2_jackson_0.wav"
        "${fixture_sources}/3_jackson_0.wav")
    idf_build_get_property(fixture_python PYTHON)
    execute_process(COMMAND "${fixture_python}" "${fixture_generator}" --output-dir "${fixture_dir}"
                    RESULT_VARIABLE fixture_result)
    if(NOT fixture_result EQUAL 0)
        message(FATAL_ERROR "Speech fixture validation/generation failed")
    endif()
    list(APPEND app_includes "${fixture_dir}")
    set(app_source "../test/test_162_audio_playback/test_main.c"
                   "${fixture_dir}/speech_fixture.c")
elseif(ORATIO_AUDIO_OUTPUT_TEST)
    set(app_source "../test/test_148_audio_output/test_main.c")
else()
    set(app_source "main.c")
endif()

idf_component_register(
    SRCS ${app_source}
    INCLUDE_DIRS ${app_includes}
    REQUIRES audio esp_driver_gpio esp_timer freertos heap log
)
