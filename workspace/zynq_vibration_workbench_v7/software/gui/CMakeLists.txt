find_package(QT NAMES Qt6 Qt5 COMPONENTS Widgets QUIET)
if(NOT QT_FOUND)
    message(WARNING "Qt Widgets was not found. GUI is disabled; vibration_analyze remains available.")
    return()
endif()

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)

add_library(vibration_gui_widgets STATIC
    main_window.cpp
    main_window.hpp
    acquisition_panel.cpp
    acquisition_panel.hpp
    plot_widget.cpp
    plot_widget.hpp
    view_math.cpp
    view_math.hpp
)
target_include_directories(vibration_gui_widgets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vibration_gui_widgets PUBLIC vibration_core Qt${QT_VERSION_MAJOR}::Widgets)
target_compile_features(vibration_gui_widgets PUBLIC cxx_std_17)

add_executable(vibration_gui ../app/gui_main.cpp)
target_link_libraries(vibration_gui PRIVATE vibration_gui_widgets)
install(TARGETS vibration_gui RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    add_executable(vibration_gui_plot_probe plot_interaction_probe.cpp)
    target_link_libraries(vibration_gui_plot_probe PRIVATE vibration_gui_widgets)
    add_test(NAME gui_plot_cache_interaction COMMAND vibration_gui_plot_probe)
    set_tests_properties(gui_plot_cache_interaction PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(vibration_gui_view_math_test view_math_test.cpp view_math.cpp)
    target_link_libraries(vibration_gui_view_math_test PRIVATE vibration_core)
    add_test(NAME gui_raw_preview_math COMMAND vibration_gui_view_math_test)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_executable(vibration_gui_workbench_probe gui_workbench_probe.cpp)
        target_link_libraries(vibration_gui_workbench_probe PRIVATE vibration_gui_widgets)
        add_test(NAME gui_workbench_interaction
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_workbench_gui.py
                --probe $<TARGET_FILE:vibration_gui_workbench_probe>
                --generator ${PROJECT_SOURCE_DIR}/tools/generate_fixture.py)
        add_executable(vibration_gui_detection_probe gui_detection_probe.cpp)
        target_link_libraries(vibration_gui_detection_probe PRIVATE vibration_gui_widgets)
        add_test(NAME gui_detection_states
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_detection_gui.py
                --probe $<TARGET_FILE:vibration_gui_detection_probe>
                --generator ${PROJECT_SOURCE_DIR}/tools/generate_fixture.py)
        add_executable(vibration_gui_probe gui_state_probe.cpp)
        target_link_libraries(vibration_gui_probe PRIVATE vibration_gui_widgets)
        add_test(NAME gui_async_states
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_gui.py
                --probe $<TARGET_FILE:vibration_gui_probe>
                --application $<TARGET_FILE:vibration_gui>
                --generator ${PROJECT_SOURCE_DIR}/tools/generate_fixture.py)
    endif()
endif()

# Independent acquisition-page QA uses the actual child service, explicitly in
# its host fixture mode; production application startup has no fixture option.
if(BUILD_TESTING AND TARGET vibration_capture_service)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_executable(vibration_gui_acquisition_probe acquisition_probe.cpp)
        target_link_libraries(vibration_gui_acquisition_probe PRIVATE vibration_gui_widgets)
        if(UNIX AND NOT CMAKE_CROSSCOMPILING)
            add_test(NAME gui_acquisition_service
                COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_acquisition_gui.py
                    --probe $<TARGET_FILE:vibration_gui_acquisition_probe>
                    --service $<TARGET_FILE:vibration_capture_service>)
            set_tests_properties(gui_acquisition_service PROPERTIES TIMEOUT 120)
        endif()
    endif()
endif()
