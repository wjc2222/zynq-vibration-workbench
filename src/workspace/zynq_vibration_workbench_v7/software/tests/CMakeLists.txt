find_package(Python3 COMPONENTS Interpreter)
add_executable(sha256_test sha256_test.cpp)
target_link_libraries(sha256_test PRIVATE vibration_core)
add_test(NAME sha256_known_vectors COMMAND sha256_test)
add_executable(candidate_model_test candidate_model_test.cpp)
target_link_libraries(candidate_model_test PRIVATE vibration_core)
add_test(NAME candidate_model_math_and_state COMMAND candidate_model_test)
add_executable(calibration_test calibration_test.cpp ../gui/view_math.cpp)
target_link_libraries(calibration_test PRIVATE vibration_core)
add_test(NAME per_channel_calibration COMMAND calibration_test)
add_executable(core_execution_test core_execution_test.cpp)
target_link_libraries(core_execution_test PRIVATE vibration_core)
add_test(NAME core_crc_and_execution COMMAND core_execution_test)
add_executable(detection_test detection_test.cpp)
target_link_libraries(detection_test PRIVATE vibration_core)
add_test(NAME detection_profile_and_gates COMMAND detection_test)
add_executable(path_protection_test path_protection_test.cpp)
target_link_libraries(path_protection_test PRIVATE vibration_core)
if(Python3_Interpreter_FOUND)
  add_test(NAME candidate_cli_provenance_and_rejection
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_candidate_cli.py
      --executable $<TARGET_FILE:vibration_detect>
      --generator ${PROJECT_SOURCE_DIR}/tools/generate_fixture.py)
  add_test(NAME path_protection_relative_missing
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_path_protection.py
      --probe $<TARGET_FILE:path_protection_test>
      --executable $<TARGET_FILE:vibration_analyze>
      --generator ${PROJECT_SOURCE_DIR}/tools/generate_fixture.py)
  add_test(NAME detection_cli
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_detection_cli.py
      --executable $<TARGET_FILE:vibration_analyze>
      --generator ${PROJECT_SOURCE_DIR}/tools/generate_fixture.py)
  add_test(NAME core_protocol_and_signal
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_core.py
      --executable $<TARGET_FILE:vibration_analyze>
      --generator ${PROJECT_SOURCE_DIR}/tools/generate_fixture.py)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING AND Python3_Interpreter_FOUND
   AND TARGET vibration_stream_service AND TARGET vibration_capture_service)
  add_library(storage_sync_fault SHARED storage_sync_fault.cpp)
  target_link_libraries(storage_sync_fault PRIVATE ${CMAKE_DL_LIBS})
  add_test(NAME storage_post_rename_sync
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_storage_sync.py
      --stream $<TARGET_FILE:vibration_stream_service>
      --capture $<TARGET_FILE:vibration_capture_service>
      --preload $<TARGET_FILE:storage_sync_fault>
      --report ${CMAKE_CURRENT_BINARY_DIR}/storage-post-rename-sync-report.json)
  set_tests_properties(storage_post_rename_sync PROPERTIES TIMEOUT 90)
endif()
add_executable(candidate_speed_domain_test candidate_speed_domain_test.cpp)
target_link_libraries(candidate_speed_domain_test PRIVATE vibration_core)
add_test(NAME candidate_speed_domain_policy COMMAND candidate_speed_domain_test)
add_executable(reference_library_test reference_library_test.cpp)
target_link_libraries(reference_library_test PRIVATE vibration_core)
add_test(NAME reference_library_math_and_provenance COMMAND reference_library_test)
add_executable(reference_ensemble_test reference_ensemble_test.cpp)
target_link_libraries(reference_ensemble_test PRIVATE vibration_core)
add_test(NAME reference_ensemble_robustness COMMAND reference_ensemble_test)
add_executable(reference_validation_test reference_validation_test.cpp)
target_link_libraries(reference_validation_test PRIVATE vibration_core)
add_test(NAME reference_validation_group_holdout COMMAND reference_validation_test)
add_executable(reference_speed_tolerant_test reference_speed_tolerant_test.cpp)
target_link_libraries(reference_speed_tolerant_test PRIVATE vibration_core)
add_test(NAME reference_speed_tolerant_policy COMMAND reference_speed_tolerant_test)
add_executable(supervised_features_test supervised_features_test.cpp)
target_link_libraries(supervised_features_test PRIVATE vibration_core)
add_test(NAME supervised_rpmfree_features COMMAND supervised_features_test)
add_executable(supervised_model_test supervised_model_test.cpp)
target_link_libraries(supervised_model_test PRIVATE vibration_core)
add_test(NAME supervised_training_and_group_holdout COMMAND supervised_model_test)
add_executable(supervised_training_probe supervised_training_probe.cpp)
target_link_libraries(supervised_training_probe PRIVATE vibration_core)
