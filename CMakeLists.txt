cmake_minimum_required(VERSION 3.16)
project(MBot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
find_program(PROTOC protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

# ---------------------------------------------------------------------------
# Vector's gRPC protocol (Apache-2.0, from the Anki/wire-pod Python SDK).
# The .proto files import google/api/annotations.proto for REST gateway hints we
# don't need, so we copy them into the build tree with those bits stripped.
# ---------------------------------------------------------------------------
set(PROTO_SRC_DIR ${CMAKE_SOURCE_DIR}/third_party/vector-sdk/anki_vector/messaging)
set(PROTO_OUT_ROOT ${CMAKE_BINARY_DIR}/proto)
set(PROTO_OUT_DIR ${PROTO_OUT_ROOT}/anki_vector/messaging)
set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${PROTO_OUT_DIR} ${GEN_DIR})

if(NOT EXISTS ${PROTO_SRC_DIR}/external_interface.proto)
  message(FATAL_ERROR "Vector protos missing. Run scripts/fetch_deps.sh first.")
endif()

set(PROTO_NAMES alexa behavior cube extensions external_interface messages
                nav_map response_status settings shared)
set(GEN_SRCS)
foreach(name ${PROTO_NAMES})
  file(READ ${PROTO_SRC_DIR}/${name}.proto content)
  string(REPLACE "import \"google/api/annotations.proto\";" "" content "${content}")
  string(REGEX REPLACE "option \\(google\\.api\\.http\\) = {([^{}]|{[^{}]*})*};" "" content "${content}")
  file(WRITE ${PROTO_OUT_DIR}/${name}.proto.tmp "${content}")
  configure_file(${PROTO_OUT_DIR}/${name}.proto.tmp ${PROTO_OUT_DIR}/${name}.proto COPYONLY)
  list(APPEND GEN_SRCS ${GEN_DIR}/anki_vector/messaging/${name}.pb.cc)
endforeach()
list(APPEND GEN_SRCS ${GEN_DIR}/anki_vector/messaging/external_interface.grpc.pb.cc)

set(PROTO_FILES)
foreach(name ${PROTO_NAMES})
  list(APPEND PROTO_FILES anki_vector/messaging/${name}.proto)
endforeach()

add_custom_command(
  OUTPUT ${GEN_SRCS}
  COMMAND ${PROTOC} -I ${PROTO_OUT_ROOT} --cpp_out=${GEN_DIR} ${PROTO_FILES}
  COMMAND ${PROTOC} -I ${PROTO_OUT_ROOT} --grpc_out=${GEN_DIR}
          --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
          anki_vector/messaging/external_interface.proto
  WORKING_DIRECTORY ${PROTO_OUT_ROOT}
  DEPENDS ${PROTO_OUT_DIR}/external_interface.proto
  COMMENT "Generating Vector gRPC stubs")

add_library(vector_proto STATIC ${GEN_SRCS})
target_include_directories(vector_proto PUBLIC ${GEN_DIR})
target_link_libraries(vector_proto PUBLIC PkgConfig::GRPCPP PkgConfig::PROTOBUF)
target_compile_options(vector_proto PRIVATE -w)

# ---------------------------------------------------------------------------
# whisper.cpp (MIT) for on-device speech recognition, built as a static lib.
# ---------------------------------------------------------------------------
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Speech-to-text on the GPU when the machine can: Vulkan (any vendor; needs the Vulkan headers/loader and
# glslc) or CUDA (needs nvcc). MBOT_GPU=OFF forces CPU. At run time the brain falls back to the CPU anyway
# if the GPU can't be used.
set(MBOT_GPU "AUTO" CACHE STRING "Whisper on the GPU: AUTO, VULKAN, CUDA or OFF")
if(NOT MBOT_GPU STREQUAL "OFF")
  find_package(Vulkan QUIET)
  find_package(SPIRV-Headers CONFIG QUIET)
  find_program(GLSLC_EXE glslc)
  find_program(NVCC_EXE nvcc)
  if((MBOT_GPU STREQUAL "AUTO" OR MBOT_GPU STREQUAL "VULKAN") AND Vulkan_FOUND AND GLSLC_EXE AND SPIRV-Headers_FOUND)
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)
    message(STATUS "speech-to-text: GPU (Vulkan)")
  elseif((MBOT_GPU STREQUAL "AUTO" OR MBOT_GPU STREQUAL "CUDA") AND NVCC_EXE)
    set(GGML_CUDA ON CACHE BOOL "" FORCE)
    message(STATUS "speech-to-text: GPU (CUDA)")
  else()
    message(STATUS "speech-to-text: CPU (for the GPU install the Vulkan headers + loader, glslc and SPIR-V "
                   "headers - scripts/install_deps.sh does - or CUDA)")
  endif()
endif()
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/whisper.cpp/CMakeLists.txt)
  message(FATAL_ERROR "whisper.cpp missing. Run scripts/fetch_deps.sh first.")
endif()
add_subdirectory(third_party/whisper.cpp EXCLUDE_FROM_ALL)

add_executable(mbot
  src/main.cpp
  src/config.cpp
  src/tls_probe.cpp
  src/net.cpp
  src/vector_client.cpp
  src/llm.cpp
  src/wav.cpp
  src/mind.cpp
  src/ears.cpp
  src/voice_id.cpp
  src/doa.cpp
  src/provision.cpp
  src/brain.cpp)
target_compile_options(mbot PRIVATE -Wall -Wextra)
target_link_libraries(mbot PRIVATE vector_proto whisper OpenSSL::SSL OpenSSL::Crypto
                      nlohmann_json::nlohmann_json Threads::Threads)

# Optional: speaker (voice) recognition via sherpa-onnx (Apache-2.0) - fetched by scripts/fetch_deps.sh.
set(SHERPA_DIR ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx)
if(EXISTS ${SHERPA_DIR}/include/sherpa-onnx/c-api/c-api.h AND EXISTS ${SHERPA_DIR}/lib/libsherpa-onnx-c-api.so)
  target_compile_definitions(mbot PRIVATE MBOT_VOICE_ID)
  target_include_directories(mbot PRIVATE ${SHERPA_DIR}/include)
  target_link_libraries(mbot PRIVATE ${SHERPA_DIR}/lib/libsherpa-onnx-c-api.so)
  set_target_properties(mbot PROPERTIES BUILD_RPATH ${SHERPA_DIR}/lib)
  message(STATUS "voice recognition: on (sherpa-onnx)")
else()
  message(STATUS "voice recognition: off (run scripts/fetch_deps.sh to enable)")
endif()
