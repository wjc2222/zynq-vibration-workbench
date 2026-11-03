find_package(Qt5 COMPONENTS Widgets QUIET)
if(NOT Qt5Widgets_FOUND)
    message(STATUS "Qt5 unavailable; the new workbench is not built")
    return()
endif()
set(CMAKE_AUTOMOC ON)
set(WORKBENCH_TEST_PLATFORM "offscreen" CACHE STRING "Qt platform for workbench host tests")
add_library(vibration_workbench_widgets STATIC
    controls.cpp controls.hpp window.cpp window.hpp
    analysis_pane.cpp analysis_pane.hpp detection_pane.cpp detection_pane.hpp
    candidate_panel.cpp candidate_panel.hpp
    record_labels.cpp record_labels.hpp
    capture_client.cpp capture_client.hpp capture_page.cpp capture_page.hpp
    capture_transport.cpp capture_transport.hpp stream_preview_contract.hpp
    plot_canvas.cpp plot_canvas.hpp
    waveform_stack.cpp waveform_stack.hpp
    single_waveform_view.cpp single_waveform_view.hpp
    live_preview_model.cpp live_preview_model.hpp
    calibration_settings.cpp calibration_settings.hpp
    touch_keyboard.cpp touch_keyboard.hpp
    touch_directory_dialog.cpp touch_directory_dialog.hpp
    ../gui/view_math.cpp ../gui/view_math.hpp)
target_include_directories(vibration_workbench_widgets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vibration_workbench_widgets PUBLIC vibration_core Qt5::Widgets)
target_compile_features(vibration_workbench_widgets PUBLIC cxx_std_17)
add_executable(vibration_workbench main.cpp)
target_link_libraries(vibration_workbench PRIVATE vibration_workbench_widgets)
install(TARGETS vibration_workbench RUNTIME DESTINATION bin)
if(BUILD_TESTING)
    add_executable(vibration_workbench_compact_layout_probe tests/compact_layout_probe.cpp)
    target_link_libraries(vibration_workbench_compact_layout_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_reference_library_probe tests/reference_library_probe.cpp)
    target_link_libraries(vibration_workbench_reference_library_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_reference_ensemble_workflow_probe tests/reference_ensemble_workflow_probe.cpp)
    target_link_libraries(vibration_workbench_reference_ensemble_workflow_probe PRIVATE vibration_workbench_widgets)
    # Test isolation: CandidatePanel's capability probe opens the accelerator,
    # whose driver release resets hardware. Never do that for software fixtures.
    if(UNIX AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_definitions(vibration_workbench_reference_ensemble_workflow_probe PRIVATE VIBRATION_FIXTURE_WRAP_ACCEL_PROBE=1)
        target_link_options(vibration_workbench_reference_ensemble_workflow_probe PRIVATE
            "-Wl,--wrap=_ZN9vibration11accelerator5probeERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE")
    endif()
    add_executable(vibration_workbench_root_window_flow_probe tests/root_window_flow_probe.cpp)
    target_link_libraries(vibration_workbench_root_window_flow_probe PRIVATE vibration_workbench_widgets)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME reference_library_workflow COMMAND vibration_workbench_reference_library_probe)
        set_tests_properties(reference_library_workflow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 90)
        add_test(NAME reference_ensemble_workflow COMMAND vibration_workbench_reference_ensemble_workflow_probe)
        set_tests_properties(reference_ensemble_workflow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 120)
        if(TARGET vibration_stream_service)
            add_test(NAME v7_root_window_flow COMMAND vibration_workbench_root_window_flow_probe
                $<TARGET_FILE:vibration_stream_service> "${CMAKE_BINARY_DIR}/test-evidence/v7-root-window-flow")
            add_test(NAME v7_compact_layout_flow COMMAND vibration_workbench_compact_layout_probe
                $<TARGET_FILE:vibration_stream_service> "${CMAKE_BINARY_DIR}/test-evidence/v7-compact-layout")
            set_tests_properties(v7_compact_layout_flow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 120)
            set_tests_properties(v7_root_window_flow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 120)
        endif()
    endif()
    add_executable(vibration_workbench_fixed_ranges_probe tests/fixed_ranges_probe.cpp)
    target_link_libraries(vibration_workbench_fixed_ranges_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_record_labels_probe tests/record_labels_probe.cpp)
    target_link_libraries(vibration_workbench_record_labels_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_record_labels_capture_probe tests/record_labels_capture_probe.cpp)
    target_link_libraries(vibration_workbench_record_labels_capture_probe PRIVATE vibration_workbench_widgets)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME new_workbench_fixed_ranges COMMAND vibration_workbench_fixed_ranges_probe)
        add_test(NAME v7_record_labels_storage COMMAND vibration_workbench_record_labels_probe)
        set_tests_properties(new_workbench_fixed_ranges v7_record_labels_storage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 60)
        if(TARGET vibration_stream_service)
            add_test(NAME v7_record_labels_capture COMMAND vibration_workbench_record_labels_capture_probe $<TARGET_FILE:vibration_stream_service>)
            set_tests_properties(v7_record_labels_capture PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 60)
        endif()
    endif()
    add_executable(vibration_workbench_candidate_panel_probe tests/candidate_panel_probe.cpp)
    target_link_libraries(vibration_workbench_candidate_panel_probe PRIVATE vibration_workbench_widgets)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME candidate_panel_workflow COMMAND vibration_workbench_candidate_panel_probe)
        set_tests_properties(candidate_panel_workflow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 90)
    endif()
    add_executable(vibration_workbench_arm_stream_ui_probe tests/arm_stream_ui_probe.cpp)
    target_link_libraries(vibration_workbench_arm_stream_ui_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_fresh_user_probe tests/fresh_user_probe.cpp)
    target_link_libraries(vibration_workbench_fresh_user_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_calibration_settings_probe tests/calibration_settings_probe.cpp)
    target_link_libraries(vibration_workbench_calibration_settings_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_calibration_integration_probe tests/calibration_integration_probe.cpp)
    target_link_libraries(vibration_workbench_calibration_integration_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_touch_directory_probe tests/touch_directory_probe.cpp)
    target_link_libraries(vibration_workbench_touch_directory_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_preview_paint_audit tests/preview_paint_audit.cpp)
    target_link_libraries(vibration_workbench_preview_paint_audit PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_live_view_exact_probe tests/live_view_exact_probe.cpp)
    target_link_libraries(vibration_workbench_live_view_exact_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_zero_centered_probe tests/zero_centered_axis_probe.cpp)
    target_link_libraries(vibration_workbench_zero_centered_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_preview_backlog_probe tests/preview_backlog_probe.cpp)
    target_link_libraries(vibration_workbench_preview_backlog_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_preview_batch_ui_probe tests/preview_batch_ui_probe.cpp)
    target_link_libraries(vibration_workbench_preview_batch_ui_probe PRIVATE vibration_workbench_widgets)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME new_workbench_preview_batch_ui COMMAND vibration_workbench_preview_batch_ui_probe)
        set_tests_properties(new_workbench_preview_batch_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 20)
        if(TARGET vibration_stream_service)
            file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-evidence/preview-backlog")
            add_test(NAME new_workbench_preview_backlog_165ms COMMAND vibration_workbench_preview_backlog_probe
                $<TARGET_FILE:vibration_stream_service> "${CMAKE_BINARY_DIR}/test-evidence/preview-backlog/165ms.json" 165 continuous)
            add_test(NAME new_workbench_preview_backlog_overflow COMMAND vibration_workbench_preview_backlog_probe
                $<TARGET_FILE:vibration_stream_service> "${CMAKE_BINARY_DIR}/test-evidence/preview-backlog/overflow.json" 2500 overflow)
            set_tests_properties(new_workbench_preview_backlog_165ms new_workbench_preview_backlog_overflow PROPERTIES TIMEOUT 15)
        endif()
        add_test(NAME new_workbench_zero_centered COMMAND vibration_workbench_zero_centered_probe)
        set_tests_properties(new_workbench_zero_centered PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 30)
        add_test(NAME new_workbench_live_view_exact COMMAND vibration_workbench_live_view_exact_probe)
        set_tests_properties(new_workbench_live_view_exact PROPERTIES TIMEOUT 60)
        add_test(NAME new_workbench_calibration_settings COMMAND vibration_workbench_calibration_settings_probe)
        add_test(NAME new_workbench_calibration_integration COMMAND vibration_workbench_calibration_integration_probe)
        add_test(NAME new_workbench_touch_directory COMMAND vibration_workbench_touch_directory_probe)
        if(TARGET storage_sync_fault)
          add_test(NAME new_workbench_touch_directory_sync_failure COMMAND vibration_workbench_touch_directory_probe --sync-failure)
          set_tests_properties(new_workbench_touch_directory_sync_failure PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM};LD_PRELOAD=$<TARGET_FILE:storage_sync_fault>" TIMEOUT 60)
        endif()
        set_tests_properties(new_workbench_calibration_settings new_workbench_calibration_integration new_workbench_touch_directory PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 60)
    endif()
    add_executable(vibration_workbench_touch_gesture_probe tests/touch_gesture_probe.cpp)
    target_link_libraries(vibration_workbench_touch_gesture_probe PRIVATE vibration_workbench_widgets)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME new_workbench_touch_gestures COMMAND vibration_workbench_touch_gesture_probe)
        set_tests_properties(new_workbench_touch_gestures PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 20)
    endif()
    add_executable(vibration_workbench_unified_layout_probe tests/unified_layout_probe.cpp)
    target_link_libraries(vibration_workbench_unified_layout_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_raw_fidelity_probe tests/raw_fidelity_probe.cpp)
    target_link_libraries(vibration_workbench_raw_fidelity_probe PRIVATE vibration_workbench_widgets)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME new_workbench_raw_fidelity COMMAND vibration_workbench_raw_fidelity_probe)
        set_tests_properties(new_workbench_raw_fidelity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 40)
    endif()
    add_executable(vibration_workbench_live_follow_probe tests/live_follow_probe.cpp)
    target_link_libraries(vibration_workbench_live_follow_probe PRIVATE vibration_workbench_widgets)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME new_workbench_live_follow COMMAND vibration_workbench_live_follow_probe)
        set_tests_properties(new_workbench_live_follow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 15)
    endif()
    add_executable(vibration_workbench_board_stream_probe tests/board_stream_probe.cpp)
    target_link_libraries(vibration_workbench_board_stream_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_stream_transport_probe tests/stream_transport_probe.cpp)
    target_link_libraries(vibration_workbench_stream_transport_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_legacy_live_model_probe tests/live_preview_probe.cpp)
    target_compile_definitions(vibration_workbench_legacy_live_model_probe PRIVATE LIVE_MODEL_ONLY=1)
    target_link_libraries(vibration_workbench_legacy_live_model_probe PRIVATE vibration_workbench_widgets)
    if(NOT CMAKE_CROSSCOMPILING AND TARGET vibration_stream_service)
        add_test(NAME new_workbench_stream_continuity_w1
            COMMAND vibration_workbench_stream_transport_probe $<TARGET_FILE:vibration_stream_service> continuity 1)
        set_tests_properties(new_workbench_stream_continuity_w1 PROPERTIES TIMEOUT 25)
        foreach(stream_mode timed stop close)
            foreach(stream_worker 0 1)
                add_test(NAME new_workbench_stream_${stream_mode}_w${stream_worker}
                    COMMAND vibration_workbench_stream_transport_probe $<TARGET_FILE:vibration_stream_service> ${stream_mode} ${stream_worker})
                set_tests_properties(new_workbench_stream_${stream_mode}_w${stream_worker} PROPERTIES TIMEOUT 25)
            endforeach()
        endforeach()
    endif()
    add_executable(vibration_workbench_single_waveform_probe tests/single_waveform_probe.cpp)
    target_link_libraries(vibration_workbench_single_waveform_probe PRIVATE vibration_workbench_widgets)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME new_workbench_single_waveform COMMAND vibration_workbench_single_waveform_probe)
        set_tests_properties(new_workbench_single_waveform PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 40)
    endif()
    add_executable(vibration_workbench_navigation_probe tests/waveform_navigation_probe.cpp)
    target_link_libraries(vibration_workbench_navigation_probe PRIVATE vibration_workbench_widgets)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME new_workbench_stack_navigation COMMAND vibration_workbench_navigation_probe --stack-only)
        set_tests_properties(new_workbench_stack_navigation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${WORKBENCH_TEST_PLATFORM}" TIMEOUT 20)
    endif()
    add_executable(vibration_workbench_parameters_probe tests/capture_parameters_probe.cpp)
    target_link_libraries(vibration_workbench_parameters_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_stream_page_probe tests/stream_page_probe.cpp)
    target_link_libraries(vibration_workbench_stream_page_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_worker_probe tests/capture_worker_probe.cpp
        capture_client.cpp capture_client.hpp capture_transport.cpp capture_transport.hpp)
    target_link_libraries(vibration_workbench_worker_probe PRIVATE vibration_core Qt5::Core)
    target_compile_features(vibration_workbench_worker_probe PRIVATE cxx_std_17)
    target_include_directories(vibration_workbench_worker_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(vibration_workbench_board_live_probe tests/board_live_probe.cpp)
    target_link_libraries(vibration_workbench_board_live_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_live_window_probe tests/live_window_probe.cpp)
    target_link_libraries(vibration_workbench_live_window_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_live_probe tests/live_preview_probe.cpp)
    target_link_libraries(vibration_workbench_live_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_plot_probe tests/plot_render_probe.cpp)
    target_link_libraries(vibration_workbench_plot_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_native_drop_probe tests/native_drop_probe.cpp)
    target_link_libraries(vibration_workbench_native_drop_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_data_probe tests/window_data_probe.cpp)
    target_link_libraries(vibration_workbench_data_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_analysis_probe tests/analysis_probe.cpp)
    target_link_libraries(vibration_workbench_analysis_probe PRIVATE vibration_workbench_widgets)
    add_executable(vibration_workbench_capture_probe tests/capture_page_probe.cpp)
    target_link_libraries(vibration_workbench_capture_probe PRIVATE vibration_workbench_widgets)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        if(NOT CMAKE_CROSSCOMPILING AND TARGET vibration_stream_service)
            add_test(NAME new_workbench_fresh_user
                COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fresh_user.py
                    --probe $<TARGET_FILE:vibration_workbench_fresh_user_probe>
                    --service $<TARGET_FILE:vibration_stream_service>
                    --generator ${PROJECT_SOURCE_DIR}/tools/generate_fixture.py
                    --evidence ${CMAKE_BINARY_DIR}/test-evidence/fresh-user)
            set_tests_properties(new_workbench_fresh_user PROPERTIES TIMEOUT 100)
        endif()
        add_test(NAME new_workbench_stream_page
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stream_page.py
                --probe $<TARGET_FILE:vibration_workbench_stream_page_probe>
                --service $<TARGET_FILE:vibration_stream_service>
                --evidence ${CMAKE_BINARY_DIR}/test-evidence/stream-page)
        add_test(NAME new_workbench_parameters
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capture_parameters.py
                --probe $<TARGET_FILE:vibration_workbench_parameters_probe>
                --service $<TARGET_FILE:vibration_capture_service>)
        add_test(NAME new_workbench_capture_worker
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capture_worker.py
                --probe $<TARGET_FILE:vibration_workbench_worker_probe>
                --service $<TARGET_FILE:vibration_capture_service>)
        add_test(NAME new_workbench_native_drop
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_drop.py
                --probe $<TARGET_FILE:vibration_workbench_native_drop_probe>)
        add_test(NAME new_workbench_window_data
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_window_data.py
                --probe $<TARGET_FILE:vibration_workbench_data_probe>
                --generator ${PROJECT_SOURCE_DIR}/tools/generate_fixture.py)
        add_test(NAME new_workbench_capture_flow
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capture_page.py
                --probe $<TARGET_FILE:vibration_workbench_capture_probe>
                --service $<TARGET_FILE:vibration_capture_service>)
    endif()
    # Coordinate hit tests share a real X11 desktop. Concurrent windows can
    # occlude each other, so keep this directory's UI workflows serial.
    if(NOT CMAKE_CROSSCOMPILING AND WORKBENCH_TEST_PLATFORM STREQUAL "xcb")
        get_property(workbench_host_tests DIRECTORY PROPERTY TESTS)
        if(workbench_host_tests)
            set_tests_properties(${workbench_host_tests} PROPERTIES RUN_SERIAL TRUE)
        endif()
    endif()
endif()
