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
  # TSF DLL 会被任意宿主进程加载；静态链接 C++ 运行库，避免新版 STL
  # 在目标机器上错误解析到旧版 VC++ Redist。
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /W4)
  if(CMAKE_GENERATOR MATCHES "Ninja")
    # cl.exe 以 UTF-8 输出中文 /showIncludes 前缀，而 CMake 在部分中文
    # Windows 环境中会把它按系统代码页再次解码，导致 Ninja 丢失头文件依赖。
    # 直接指定 UTF-8 前缀，确保修改类布局后所有依赖目标都会重编译。
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含文件: ")
  endif()
endif()

set(SHURU_ENGINE_FEATURE_SOURCES
  src/engine/custom_phrase.cpp
  src/engine/pinyin_correction.cpp
  src/engine/special_input.cpp
  src/engine/system_language_model.cpp
  src/engine/system_lexeme_prior.cpp
)

set(SHURU_CLIPBOARD_SOURCES
  src/engine/clipboard_helper.cpp
)

add_library(ShuruIme SHARED
  src/common/logger.cpp
  src/common/runtime_config.cpp
  src/common/typing_stats.cpp
  src/common/private_acl.cpp
  src/engine/dictionary.cpp
  src/engine/lexicon_cache.cpp
  src/engine/english_dict.cpp
  src/engine/pinyin_engine.cpp
  ${SHURU_ENGINE_FEATURE_SOURCES}
  ${SHURU_CLIPBOARD_SOURCES}
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
  src/ime/ui/directwrite_text_renderer.cpp
  src/ime/ui/skin_manager.cpp
  src/ime/dllmain.cpp
  src/ime/ShuruIme.def
  src/ime/ShuruIme.rc
)

# Windows 资源编译器不会稳定地把 rc 内部 #include 写入 Ninja 依赖图。
# 显式声明版本头与图标文件，确保修改版本号或图标时也会重编资源并重新链接 DLL。
set_property(SOURCE src/ime/ShuruIme.rc APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/common/version.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ime/app.ico")

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
  gdiplus
  dwrite
  advapi32
  bcrypt
  shell32
  dwmapi
  winsqlite3
)

add_custom_target(copy_runtime_lexicon ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/data/lexicon
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/data/lexicon
          ${CMAKE_CURRENT_BINARY_DIR}/data/lexicon
  COMMENT "Copying the shared runtime lexicon"
)
add_dependencies(ShuruIme copy_runtime_lexicon)

add_custom_target(copy_runtime_skins ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/data/skins
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/data/skins
          ${CMAKE_CURRENT_BINARY_DIR}/data/skins
  COMMENT "Copying the shared runtime skins"
)
add_dependencies(ShuruIme copy_runtime_skins)

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

  add_executable(typing_stats_test
    tests/engine_playground/typing_stats_test.cpp
    src/common/typing_stats.cpp
  )
  target_include_directories(typing_stats_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(typing_stats_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  add_test(NAME typing_stats COMMAND typing_stats_test)
  set_tests_properties(typing_stats PROPERTIES TIMEOUT 30)

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

  add_executable(english_dictionary_test
    tests/engine_playground/english_dict_test.cpp
    src/common/logger.cpp
    src/common/runtime_config.cpp
    src/engine/english_dict.cpp
  )
  target_include_directories(english_dictionary_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(english_dictionary_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  add_test(
    NAME english_dictionary
    COMMAND english_dictionary_test ${CMAKE_CURRENT_SOURCE_DIR}/data/lexicon/en_dict.txt
  )

  add_executable(english_ranking_test
    tests/engine_playground/english_ranking_test.cpp
    src/common/logger.cpp
    src/common/runtime_config.cpp
    src/common/private_acl.cpp
    src/engine/dictionary.cpp
    src/engine/lexicon_cache.cpp
    src/engine/english_dict.cpp
    src/engine/pinyin_engine.cpp
    ${SHURU_ENGINE_FEATURE_SOURCES}
  )
  target_include_directories(english_ranking_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(english_ranking_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(english_ranking_test PRIVATE user32 bcrypt)
  add_test(
    NAME english_ranking
    COMMAND english_ranking_test ${CMAKE_CURRENT_SOURCE_DIR}/data/lexicon
  )
  set_tests_properties(english_ranking PROPERTIES
    ENVIRONMENT "LOCALAPPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-userdata-english-ranking"
  )

  add_executable(system_language_model_test
    tests/engine_playground/system_language_model_test.cpp
    src/engine/system_language_model.cpp
  )
  target_include_directories(system_language_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(system_language_model_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/lexicon/rime-moqi-zh.gram")
    add_test(
      NAME system_language_model
      COMMAND system_language_model_test
              ${CMAKE_CURRENT_SOURCE_DIR}/data/lexicon/rime-moqi-zh.gram
    )
  else()
    add_test(NAME system_language_model COMMAND system_language_model_test)
  endif()
  set_tests_properties(system_language_model PROPERTIES TIMEOUT 30)

  add_executable(system_lexeme_prior_test
    tests/engine_playground/system_lexeme_prior_test.cpp
    src/engine/system_lexeme_prior.cpp
  )
  target_include_directories(system_lexeme_prior_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(system_lexeme_prior_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  add_test(NAME system_lexeme_prior COMMAND system_lexeme_prior_test)
  set_tests_properties(system_lexeme_prior PROPERTIES TIMEOUT 30)

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
    # 白霜正式词库会在本测试内执行文本装载、缓存生成、缓存校验和引擎装载；
    # 查询延迟仍由测试内部的 P99 阈值单独约束。
    # 白霜 18 MB 文本词库的冷解析与缓存重建在实时防护繁忙时可能超过
    # 300 秒；查询延迟仍由测试内部的 P99 门槛严格约束。
    TIMEOUT 600
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
    src/common/typing_stats.cpp
    src/common/private_acl.cpp
    src/engine/dictionary.cpp
  src/engine/lexicon_cache.cpp
    src/engine/english_dict.cpp
    src/engine/pinyin_engine.cpp
    ${SHURU_ENGINE_FEATURE_SOURCES}
    ${SHURU_CLIPBOARD_SOURCES}
    src/engine/shared_engine.cpp
    src/ime/globals.cpp
    src/ime/edit_sessions.cpp
    src/ime/input_policy.cpp
    src/ime/punctuation_state.cpp
    src/ime/display_attribute.cpp
    src/ime/ui/candidate_window.cpp
    src/ime/ui/directwrite_text_renderer.cpp
    src/ime/ui/skin_manager.cpp
    src/ime/ui/status_window.cpp
    src/ime/ui/shared_status_ui.cpp
    src/ime/text_service.cpp
  )
  target_include_directories(shared_status_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(shared_status_ui_test PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    SHURU_TRAY_OWNER_MUTEX_TEST=1
    SHURU_TEST_DISABLE_EXTERNAL_HELPERS=1)
  target_link_libraries(shared_status_ui_test PRIVATE ole32 oleaut32 uuid user32 gdi32 gdiplus dwrite advapi32 bcrypt shell32 dwmapi winsqlite3)

  add_test(NAME shared_status_ui COMMAND shared_status_ui_test)
  set_tests_properties(shared_status_ui PROPERTIES TIMEOUT 30)

  add_executable(clipboard_helper_test
    tests/engine_playground/clipboard_helper_test.cpp
    src/common/logger.cpp
    src/common/runtime_config.cpp
    src/engine/custom_phrase.cpp
    ${SHURU_CLIPBOARD_SOURCES}
  )
  target_include_directories(clipboard_helper_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(clipboard_helper_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(clipboard_helper_test PRIVATE ole32 advapi32 winsqlite3)
  add_test(NAME clipboard_helper COMMAND clipboard_helper_test)
  set_tests_properties(clipboard_helper PROPERTIES TIMEOUT 30)

  add_executable(candidate_window_test
    tests/engine_playground/candidate_window_test.cpp
    src/common/runtime_config.cpp
    src/common/typing_stats.cpp
    src/ime/ui/candidate_window.cpp
    src/ime/ui/directwrite_text_renderer.cpp
    src/ime/ui/skin_manager.cpp
  )
  target_include_directories(candidate_window_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(candidate_window_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(candidate_window_test PRIVATE user32 gdi32 gdiplus dwrite ole32 shell32 dwmapi)
  add_test(NAME candidate_window COMMAND candidate_window_test)
  set_tests_properties(candidate_window PROPERTIES TIMEOUT 30)

  add_executable(product_name_test
    tests/engine_playground/product_name_test.cpp
    src/common/runtime_config.cpp
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
    # 完整白霜词库较大，部署测试会多次复制并校验哈希。
    # 在机械盘或实时防护开启时 60 秒不足，保留明确上限但给足正常 I/O 时间。
    TIMEOUT 180
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
    add_test(NAME settings_ui_smoke
      COMMAND ${DOTNET_EXECUTABLE} run --project
              ${CMAKE_CURRENT_SOURCE_DIR}/settings/ShuruSettings.csproj
              --configuration Release -- -self-test)
    set_tests_properties(settings_ui_smoke PROPERTIES
      TIMEOUT 30
      ENVIRONMENT "CAISHEN_CLIPBOARD_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-settings-ui/clipboard")
  endif()
endif()
