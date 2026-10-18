cmake_minimum_required(VERSION 3.21)
project(NovaChangJie VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(NOVA_BUILD_TESTS "Build the engine unit tests" ON)
option(NOVA_BUILD_DICT "Compile data/sources into nova.dict" ON)
option(NOVA_INSTALL_SHARED_FILES "Install nova.dict and the settings app (once per package)" ON)
set(NOVA_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/sources" CACHE PATH "Directory with table.txt and essay.txt")
set(NOVA_MIN_PHRASE_WEIGHT 300 CACHE STRING "Drop corpus words below this frequency")
set(NOVA_CORPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/corpus" CACHE PATH "Directory with train.txt and laws.txt (tools/prepare-corpus.py)")
set(NOVA_EVERYDAY_WEIGHT 10 CACHE STRING "How many times everyday training text counts against law text")

if(MSVC)
  # Static CRT: the DLL is loaded into every process, so it must not depend
  # on a redistributable being installed.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /guard:cf /sdl)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  add_link_options(/guard:cf /DYNAMICBASE /NXCOMPAT)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_GENERATOR_PLATFORM MATCHES "ARM64")
    # Hardware-enforced stack protection: hardened processes (browsers,
    # sandboxed apps) refuse to load modules that are not CET compatible.
    add_link_options(/CETCOMPAT)
  endif()
else()
  add_compile_options(-Wall -Wextra)
endif()

# Architecture folder used by the package layout: <prefix>/<arch>/NovaChangJie.dll
if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64")
  set(NOVA_ARCH arm64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(NOVA_ARCH x64)
else()
  set(NOVA_ARCH x86)
endif()

# --- Portable engine ---------------------------------------------------------
add_library(nova_engine STATIC
  src/engine/Dictionary.cpp
  src/engine/DictBuilder.cpp
  src/engine/Engine.cpp
  src/engine/EnginePrefix.cpp
  src/engine/EngineSentence.cpp
  src/engine/Unicode.cpp
  src/engine/UserModel.cpp)
target_include_directories(nova_engine PUBLIC src/engine)

add_executable(ncj-dictbuild src/tools/DictBuildMain.cpp)
target_link_libraries(ncj-dictbuild PRIVATE nova_engine)

add_executable(ncj-eval src/tools/EvalMain.cpp)
target_link_libraries(ncj-eval PRIVATE nova_engine)
if(WIN32)
  target_link_libraries(ncj-eval PRIVATE nova_common)  # --neural talks to NovaChangJieLM.
  target_compile_definitions(ncj-eval PRIVATE NOVA_EVAL_NEURAL)
endif()

if(NOVA_BUILD_TESTS)
  enable_testing()
  add_executable(nova_engine_tests tests/EngineTests.cpp)
  target_link_libraries(nova_engine_tests PRIVATE nova_engine)
  add_test(NAME engine COMMAND nova_engine_tests)
endif()

# --- Dictionary ----------------------------------------------------------------
set(NOVA_DICT "${CMAKE_CURRENT_BINARY_DIR}/nova.dict")
if(NOVA_BUILD_DICT)
  if(EXISTS "${NOVA_DATA_DIR}/table.txt")
    set(_dict_args --table "${NOVA_DATA_DIR}/table.txt" --min-weight ${NOVA_MIN_PHRASE_WEIGHT})
    set(_dict_deps "${NOVA_DATA_DIR}/table.txt")
    if(EXISTS "${NOVA_DATA_DIR}/essay.txt")
      list(APPEND _dict_args --corpus "${NOVA_DATA_DIR}/essay.txt")
      list(APPEND _dict_deps "${NOVA_DATA_DIR}/essay.txt")
    else()
      message(WARNING "essay.txt not found: building without word input, association and frequency order")
    endif()
    # Cantonese words the dictionary lacks (rime-cantonese, tools/prepare-corpus.py).
    if(EXISTS "${NOVA_CORPUS_DIR}/essay-cantonese.txt")
      list(APPEND _dict_args --extra-words "${NOVA_CORPUS_DIR}/essay-cantonese.txt")
      list(APPEND _dict_deps "${NOVA_CORPUS_DIR}/essay-cantonese.txt")
    endif()
    # Word pair model for 智慧選字 (tools/prepare-corpus.py). Everyday text
    # counts more than law text, which is far larger but very formal.
    if(EXISTS "${NOVA_CORPUS_DIR}/train.txt")
      list(APPEND _dict_args --weight ${NOVA_EVERYDAY_WEIGHT} --text "${NOVA_CORPUS_DIR}/train.txt")
      list(APPEND _dict_deps "${NOVA_CORPUS_DIR}/train.txt")
    endif()
    if(EXISTS "${NOVA_CORPUS_DIR}/laws.txt")
      list(APPEND _dict_args --weight 1 --text "${NOVA_CORPUS_DIR}/laws.txt")
      list(APPEND _dict_deps "${NOVA_CORPUS_DIR}/laws.txt")
    endif()
    add_custom_command(
      OUTPUT "${NOVA_DICT}"
      COMMAND ncj-dictbuild ${_dict_args} --out "${NOVA_DICT}"
      DEPENDS ncj-dictbuild ${_dict_deps}
      COMMENT "Compiling nova.dict"
      VERBATIM)
    add_custom_target(nova_dict ALL DEPENDS "${NOVA_DICT}")
  else()
    message(WARNING "No ${NOVA_DATA_DIR}/table.txt - run tools/fetch-data.ps1 first. "
                    "The input method cannot type without nova.dict.")
    set(NOVA_BUILD_DICT OFF)
  endif()
endif()

# --- Windows text service and settings app -----------------------------------
if(WIN32)
  enable_language(RC)

  add_library(nova_common STATIC src/common/SettingsStore.cpp src/common/LmClient.cpp src/common/UserWordsData.cpp)
  target_include_directories(nova_common PUBLIC src/common)
  target_link_libraries(nova_common PUBLIC nova_engine advapi32 shell32 ole32 uuid)

  add_library(NovaChangJie SHARED
    src/tip/CandidateUI.cpp
    src/tip/DisplayAttribute.cpp
    src/tip/DllMain.cpp
    src/tip/Globals.cpp
    src/tip/LangBarButton.cpp
    src/tip/Register.cpp
    src/tip/SharedData.cpp
    src/tip/TextService.cpp
    src/tip/NovaChangJie.rc
    src/tip/NovaChangJie.def)
  target_include_directories(NovaChangJie PRIVATE src/tip)
  target_link_libraries(NovaChangJie PRIVATE
    nova_engine nova_common uuid ole32 oleaut32 user32 gdi32 advapi32 shell32 d2d1 dwrite dwmapi)

  add_executable(NovaChangJieSettings WIN32
    src/settings/SettingsMain.cpp
    src/settings/UserWords.cpp
    src/settings/Settings.rc
    src/settings/Settings.manifest)
  target_link_libraries(NovaChangJieSettings PRIVATE nova_common comctl32 comdlg32 shell32 ole32 user32 gdi32)

  install(TARGETS NovaChangJie RUNTIME DESTINATION ${NOVA_ARCH} LIBRARY DESTINATION ${NOVA_ARCH})
  if(NOVA_INSTALL_SHARED_FILES)
    install(TARGETS NovaChangJieSettings RUNTIME DESTINATION .)
    if(NOVA_BUILD_DICT)
      install(FILES "${NOVA_DICT}" DESTINATION .)
    endif()
    install(FILES installer/install.ps1 installer/uninstall.ps1 DESTINATION .)
    install(FILES LICENSE README.md README.zh-TW.md DESTINATION .)
    if(EXISTS "${NOVA_DATA_DIR}/SOURCES.md")
      install(FILES "${NOVA_DATA_DIR}/SOURCES.md" DESTINATION . RENAME DATA-LICENSES.md)
    endif()
    if(EXISTS "${NOVA_CORPUS_DIR}/SOURCES.md")
      # Attribution for the text the word pair model and nova-lm were trained on.
      install(FILES "${NOVA_CORPUS_DIR}/SOURCES.md" DESTINATION . RENAME TEXT-SOURCES.md)
    endif()
  endif()

  # NovaChangJieLM: neural sentence scorer (x64 helper process, serves every
  # architecture of the text service through a named pipe).
  set(NOVA_ORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/onnxruntime/onnxruntime-win-x64-1.30.0"
      CACHE PATH "ONNX Runtime (tools/fetch-data.ps1 -Neural)")
  set(NOVA_LM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/lm" CACHE PATH "nova-lm.onnx and nova-lm.vocab (tools/train-lm.py)")
  if(NOVA_ARCH STREQUAL "x64" AND EXISTS "${NOVA_ORT_DIR}/include/onnxruntime_cxx_api.h")
    add_executable(NovaChangJieLM WIN32 src/lm/LmMain.cpp)
    target_include_directories(NovaChangJieLM PRIVATE "${NOVA_ORT_DIR}/include")
    target_link_libraries(NovaChangJieLM PRIVATE nova_common "${NOVA_ORT_DIR}/lib/onnxruntime.lib")
    if(NOVA_INSTALL_SHARED_FILES)
      install(TARGETS NovaChangJieLM RUNTIME DESTINATION lm)
      install(FILES "${NOVA_ORT_DIR}/lib/onnxruntime.dll" DESTINATION lm)
      install(FILES "${NOVA_ORT_DIR}/LICENSE" DESTINATION lm RENAME ONNXRUNTIME-LICENSE.txt)
      install(FILES "${NOVA_ORT_DIR}/ThirdPartyNotices.txt" DESTINATION lm RENAME ONNXRUNTIME-ThirdPartyNotices.txt)
      if(EXISTS "${NOVA_LM_DIR}/nova-lm.onnx")
        install(FILES "${NOVA_LM_DIR}/nova-lm.onnx" "${NOVA_LM_DIR}/nova-lm.vocab" DESTINATION lm)
      else()
        message(WARNING "No ${NOVA_LM_DIR}/nova-lm.onnx - run tools/train-lm.py; NovaChangJieLM will have no model.")
      endif()
    endif()
  endif()
endif()
