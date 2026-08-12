cmake_minimum_required(VERSION 3.20)
project(ShuruPinyin LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "Shuru Pinyin IME only supports Windows.")
endif()
if(MSVC)
  add_compile_options(/utf-8 /W4)
  if(CMAKE_GENERATOR MATCHES "Ninja")
    # cl.exe 以 UTF-8 输出中文 /showIncludes 前缀，而 CMake 在部分中文
    # Windows 环境中会把它按系统代码页再次解码，导致 Ninja 丢失头文件依赖。
    # 直接指定 UTF-8 前缀，确保修改类布局后所有依赖目标都会重编译。
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件: ")
  endif()
endif()

set(SHURU_ENGINE_FEATURE_SOURCES
  src/engine/pinyin_correction.cpp
  src/engine/special_input.cpp
)

add_library(ShuruIme SHARED
  src/common/logger.cpp
  src/common/runtime_config.cpp
  src/common/private_acl.cpp
  src/engine/dictionary.cpp
  src/engine/lexicon_cache.cpp
  src/engine/english_dict.cpp
  src/engine/pinyin_engine.cpp
  ${SHURU_ENGINE_FEATURE_SOURCES}
  src/engine/shared_engine.cpp
  src/ime/globals.cpp
  src/ime/class_factory.cpp
  src/ime/register.cpp
  src/ime/edit_sessions.cpp
  src/ime/input_policy.cpp
  src/ime/punctuation_state.cpp
  src/ime/display_attribute.cpp
  src/ime/ui/status_window.cpp
  src/ime/ui/shared_status_ui.cpp
  src/ime/text_service.cpp
  src/ime/ui/candidate_window.cpp
  src/ime/dllmain.cpp
  src/ime/ShuruIme.def
  src/ime/ShuruIme.rc
)

target_include_directories(ShuruIme PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(ShuruIme PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

target_link_libraries(ShuruIme PRIVATE
  ole32
  oleaut32
  uuid
  user32
  gdi32
  advapi32
  bcrypt
)

add_custom_target(copy_runtime_lexicon ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/data/lexicon
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/data/lexicon
          ${CMAKE_CURRENT_BINARY_DIR}/data/lexicon
  COMMENT "Copying the shared runtime lexicon"
)
add_dependencies(ShuruIme copy_runtime_lexicon)

add_executable(engine_playground
  tests/engine_playground/main.cpp
  src/common/logger.cpp
  src/common/runtime_config.cpp
  src/common/private_acl.cpp
  src/engine/dictionary.cpp
  src/engine/lexicon_cache.cpp
  src/engine/english_dict.cpp
  src/engine/pinyin_engine.cpp
  ${SHURU_ENGINE_FEATURE_SOURCES}
  src/engine/shared_engine.cpp
)

target_include_directories(engine_playground PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(engine_playground PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(engine_playground PRIVATE user32 bcrypt)

add_dependencies(engine_playground copy_runtime_lexicon)

if(BUILD_TESTING)
  add_executable(input_policy_test
    tests/engine_playground/input_policy_test.cpp
    src/ime/input_policy.cpp
    src/ime/punctuation_state.cpp
  )
  target_include_directories(input_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(input_policy_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  add_test(NAME input_policy COMMAND input_policy_test)

  add_executable(special_input_test
    tests/engine_playground/special_input_test.cpp
    ${SHURU_ENGINE_FEATURE_SOURCES}
  )
  target_include_directories(special_input_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(special_input_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  add_test(NAME special_input COMMAND special_input_test)

  add_executable(runtime_config_test
    tests/engine_playground/runtime_config_test.cpp
    src/common/runtime_config.cpp
  )
  target_include_directories(runtime_config_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(runtime_config_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  add_test(NAME runtime_config COMMAND runtime_config_test)
  set_tests_properties(runtime_config PROPERTIES
    ENVIRONMENT "LOCALAPPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-runtime-config"
  )

  add_executable(engine_phase1_test
    tests/engine_playground/phase1_test.cpp
    src/common/logger.cpp
    src/common/runtime_config.cpp
    src/common/private_acl.cpp
    src/engine/dictionary.cpp
  src/engine/lexicon_cache.cpp
    src/engine/english_dict.cpp
    src/engine/pinyin_engine.cpp
    ${SHURU_ENGINE_FEATURE_SOURCES}
  )
  target_include_directories(engine_phase1_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(engine_phase1_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(engine_phase1_test PRIVATE user32 bcrypt)

  add_test(
    NAME engine_phase1
    COMMAND engine_phase1_test ${CMAKE_CURRENT_SOURCE_DIR}/data/lexicon
  )
  set_tests_properties(engine_phase1 PROPERTIES
    ENVIRONMENT "LOCALAPPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-userdata"
  )

  add_executable(p1_engine_test
    tests/engine_playground/p1_engine_test.cpp
    src/common/logger.cpp
    src/common/runtime_config.cpp
    src/common/private_acl.cpp
    src/engine/dictionary.cpp
    src/engine/lexicon_cache.cpp
    src/engine/english_dict.cpp
    src/engine/pinyin_engine.cpp
    ${SHURU_ENGINE_FEATURE_SOURCES}
  )
  target_include_directories(p1_engine_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(p1_engine_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(p1_engine_test PRIVATE user32 bcrypt)
  add_test(NAME p1_engine COMMAND p1_engine_test ${CMAKE_CURRENT_SOURCE_DIR}/data/lexicon)
  set_tests_properties(p1_engine PROPERTIES
    ENVIRONMENT "LOCALAPPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-userdata-p1"
    TIMEOUT 60
  )

  add_executable(shared_engine_lifecycle_test
    tests/engine_playground/shared_engine_lifecycle_test.cpp
    src/common/logger.cpp
    src/common/runtime_config.cpp
    src/common/private_acl.cpp
    src/engine/dictionary.cpp
  src/engine/lexicon_cache.cpp
    src/engine/english_dict.cpp
    src/engine/pinyin_engine.cpp
    ${SHURU_ENGINE_FEATURE_SOURCES}
    src/engine/shared_engine.cpp
  )
  target_include_directories(shared_engine_lifecycle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(shared_engine_lifecycle_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(shared_engine_lifecycle_test PRIVATE user32 bcrypt)

  add_test(NAME shared_engine_lifecycle COMMAND shared_engine_lifecycle_test)
  set_tests_properties(shared_engine_lifecycle PROPERTIES
    ENVIRONMENT "LOCALAPPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-userdata-lifecycle"
    TIMEOUT 30
  )

  add_executable(dictionary_stress_test
    tests/engine_playground/dictionary_stress_test.cpp
    src/common/logger.cpp
    src/common/runtime_config.cpp
    src/common/private_acl.cpp
    src/engine/dictionary.cpp
  src/engine/lexicon_cache.cpp
    src/engine/english_dict.cpp
    src/engine/pinyin_engine.cpp
    ${SHURU_ENGINE_FEATURE_SOURCES}
  )
  target_include_directories(dictionary_stress_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(dictionary_stress_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(dictionary_stress_test PRIVATE user32 bcrypt)

  add_test(NAME dictionary_stress COMMAND dictionary_stress_test)
  set_tests_properties(dictionary_stress PROPERTIES
    ENVIRONMENT "LOCALAPPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-userdata-stress"
    TIMEOUT 30
  )

  add_executable(logger_test
    tests/engine_playground/logger_test.cpp
    src/common/logger.cpp
    src/common/runtime_config.cpp
  )
  target_include_directories(logger_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(logger_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

  add_test(NAME logger_rotation COMMAND logger_test)
  set_tests_properties(logger_rotation PROPERTIES TIMEOUT 30)

  add_executable(dll_smoke_test
    tests/engine_playground/dll_smoke_test.cpp
  )
  target_include_directories(dll_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(dll_smoke_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(dll_smoke_test PRIVATE ole32 uuid)

  add_test(NAME dll_smoke COMMAND dll_smoke_test $<TARGET_FILE:ShuruIme>)
  set_tests_properties(dll_smoke PROPERTIES TIMEOUT 30)

  add_executable(shared_status_ui_test
    tests/engine_playground/shared_status_ui_test.cpp
    src/common/logger.cpp
    src/common/runtime_config.cpp
    src/common/private_acl.cpp
    src/engine/dictionary.cpp
  src/engine/lexicon_cache.cpp
    src/engine/english_dict.cpp
    src/engine/pinyin_engine.cpp
    ${SHURU_ENGINE_FEATURE_SOURCES}
    src/engine/shared_engine.cpp
    src/ime/globals.cpp
    src/ime/edit_sessions.cpp
    src/ime/input_policy.cpp
    src/ime/punctuation_state.cpp
    src/ime/display_attribute.cpp
    src/ime/ui/candidate_window.cpp
    src/ime/ui/status_window.cpp
    src/ime/ui/shared_status_ui.cpp
    src/ime/text_service.cpp
  )
  target_include_directories(shared_status_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(shared_status_ui_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(shared_status_ui_test PRIVATE ole32 oleaut32 uuid user32 gdi32 advapi32 bcrypt)

  add_test(NAME shared_status_ui COMMAND shared_status_ui_test)
  set_tests_properties(shared_status_ui PROPERTIES TIMEOUT 30)

  add_executable(candidate_window_test
    tests/engine_playground/candidate_window_test.cpp
    src/common/runtime_config.cpp
    src/ime/ui/candidate_window.cpp
  )
  target_include_directories(candidate_window_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(candidate_window_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(candidate_window_test PRIVATE user32 gdi32)
  add_test(NAME candidate_window COMMAND candidate_window_test)
  set_tests_properties(candidate_window PROPERTIES TIMEOUT 30)

  add_executable(product_name_test
    tests/engine_playground/product_name_test.cpp
  )
  target_include_directories(product_name_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(product_name_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(product_name_test PRIVATE advapi32)

  add_test(NAME product_name COMMAND product_name_test)
  set_tests_properties(product_name PROPERTIES TIMEOUT 30)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME lexicon_manifest
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/lexicon_manifest.py validate
              --dir ${CMAKE_CURRENT_SOURCE_DIR}/data/lexicon
              --manifest ${CMAKE_CURRENT_SOURCE_DIR}/data/lexicon/manifest.json)
  endif()
  add_test(NAME deployment_core
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/deployment_core_test.ps1)
  set_tests_properties(deployment_core PROPERTIES
    TIMEOUT 60
    # VsDevCmd 可能把 PowerShell 7 模块目录放到最前，导致 Windows PowerShell
    # 先发现不兼容的 Microsoft.PowerShell.Utility，而不是自身模块。
    ENVIRONMENT "PSModulePath=$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/Modules"
  )

  add_executable(release_health_check
    tests/release_health_check.cpp
    src/common/logger.cpp
    src/common/runtime_config.cpp
    src/common/private_acl.cpp
    src/engine/dictionary.cpp
    src/engine/lexicon_cache.cpp
    src/engine/english_dict.cpp
    src/engine/pinyin_engine.cpp
    ${SHURU_ENGINE_FEATURE_SOURCES}
  )
  target_include_directories(release_health_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(release_health_check PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(release_health_check PRIVATE ole32 oleaut32 uuid user32 advapi32 bcrypt)
  add_test(NAME release_health
    COMMAND release_health_check $<TARGET_FILE:ShuruIme> ${CMAKE_CURRENT_SOURCE_DIR}/data/lexicon)
  set_tests_properties(release_health PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "LOCALAPPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-userdata-release-health"
  )

  add_executable(tsf_e2e_host tests/tsf_e2e_host.cpp)
  target_include_directories(tsf_e2e_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(tsf_e2e_host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(tsf_e2e_host PRIVATE ole32 oleaut32 uuid user32)

  add_executable(tsf_e2e_core tests/tsf_e2e_core.cpp
    src/ime/edit_sessions.cpp src/ime/display_attribute.cpp src/ime/input_policy.cpp
    src/common/logger.cpp src/common/runtime_config.cpp)
  target_include_directories(tsf_e2e_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(tsf_e2e_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(tsf_e2e_core PRIVATE ole32 oleaut32 uuid user32 advapi32)
  add_test(NAME tsf_e2e_core COMMAND tsf_e2e_core)
  set_tests_properties(tsf_e2e_core PROPERTIES TIMEOUT 60)

  find_program(DOTNET_EXECUTABLE dotnet)
  if(DOTNET_EXECUTABLE)
    add_test(NAME settings_logic
      COMMAND ${DOTNET_EXECUTABLE} run --project
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/settings_logic/SettingsLogic.csproj
              --configuration Release)
    set_tests_properties(settings_logic PROPERTIES TIMEOUT 60)
  endif()
endif()
