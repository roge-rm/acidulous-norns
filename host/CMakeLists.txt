# Builds the Acidulous engine as a SuperCollider plugin, and a desktop render
# tool that runs the same core offline. The engine's source is in
# third_party/acidulous, filled by tools/vendor-acidulous.sh.
cmake_minimum_required(VERSION 3.16)
project(acidulous_host CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The app builds the engine this way.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -ffast-math -fno-finite-math-only")

# norns runs 32-bit ARM on a Pi 3 (Cortex-A53) or Pi 4 (Cortex-A72), both ARMv8.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
    add_compile_options(-mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard)
endif()

set(ACID ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/acidulous)
set(E ${ACID}/engine)
find_package(Threads REQUIRED)
find_path(LO_INC lo/lo.h)
find_library(LO_LIB lo)
# The SuperCollider plugin needs SC's plugin headers.
find_path(SC_INC SC_PlugIn.h PATH_SUFFIXES SuperCollider/plugin_interface)

# The same files as the app's tools/host_engine.sh: every machine, effect and
# dsp file and the parts of core, rack and format the Engine needs. No Link,
# LAME or platform code.
file(GLOB_RECURSE engine_src ${E}/machine/*.cpp ${E}/dsp/*.cpp ${E}/effect/*.cpp ${E}/inputmod/*.cpp)
list(APPEND engine_src
    ${E}/core/Utterance.cpp ${E}/core/Take.cpp ${E}/core/SampleEdit.cpp ${E}/core/Tuner.cpp
    ${E}/core/Capture.cpp ${E}/core/ReelCache.cpp
    ${E}/rack/Rack.cpp ${E}/rack/Engine.cpp ${E}/rack/TrackPool.cpp ${E}/rack/MasterBus.cpp
    ${E}/format/WavStream.cpp ${E}/format/WavWriter.cpp ${E}/format/WavReader.cpp
    ${E}/format/Decoded.cpp ${E}/format/AiffReader.cpp ${E}/format/FlacReader.cpp
    ${E}/format/AudioDecoder.cpp)

add_library(acidulous-engine STATIC ${engine_src})
target_include_directories(acidulous-engine PUBLIC ${ACID})
target_compile_options(acidulous-engine PRIVATE -w)
target_link_libraries(acidulous-engine PUBLIC Threads::Threads)
# Also linked into the SuperCollider plugin.
set_property(TARGET acidulous-engine PROPERTY POSITION_INDEPENDENT_CODE ON)

add_library(acidulous-core STATIC src/core.cpp)
target_compile_options(acidulous-core PRIVATE -Wall)
target_link_libraries(acidulous-core PUBLIC acidulous-engine)
set_property(TARGET acidulous-core PROPERTY POSITION_INDEPENDENT_CODE ON)

add_executable(acidulous-render src/render.cpp)
target_compile_options(acidulous-render PRIVATE -Wall)
target_compile_definitions(acidulous-render PRIVATE
    ACIDULOUS_DEFAULT_BANKS="${CMAKE_CURRENT_SOURCE_DIR}/../data/banks")
target_link_libraries(acidulous-render acidulous-core)

if(SC_INC AND LO_INC AND LO_LIB)
    # scsynth loads every plugin into one process, so nothing but the plugin
    # entry points may be visible.
    add_library(Acidulous MODULE src/ugen.cpp src/bridge.cpp)
    set_target_properties(Acidulous PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
    target_compile_definitions(Acidulous PRIVATE ACIDULOUS_OSC_PORT=57150)
    target_compile_options(Acidulous PRIVATE -Wall)
    target_include_directories(Acidulous PRIVATE ${SC_INC} ${SC_INC}/../common ${LO_INC})
    target_link_libraries(Acidulous acidulous-core ${LO_LIB})
    target_link_options(Acidulous PRIVATE -Wl,--exclude-libs,ALL -Wl,-Bsymbolic)
else()
    message(STATUS "No SuperCollider plugin headers or liblo: building only acidulous-render")
endif()
