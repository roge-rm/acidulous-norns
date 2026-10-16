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

# The engine as the app's desktop and web builds compile it: every engine
# source, the app's EngineHost and its JNI bridge (called through the web
# build's fake JNI), with Android's log going to stderr and Link always off.
# The audio driver is our own, which opens no device: the UGen pulls blocks
# through it. MP3 is stubbed to an error instead of building LAME.
file(GLOB_RECURSE engine_src ${E}/*.cpp)
list(FILTER engine_src EXCLUDE REGEX "/format/Mp3(Reader|Writer)\\.cpp$")
list(APPEND engine_src
    ${ACID}/EngineHost.cpp
    ${ACID}/platform/android/jni_bridge.cpp
    ${ACID}/platform/web/LinkOff.cpp
    src/norns/drivers/AudioDriver.cpp
    src/norns/Mp3Off.cpp)

add_library(acidulous-engine STATIC ${engine_src})
# Our driver first, so EngineHost's <drivers/AudioDriver.h> is ours.
target_include_directories(acidulous-engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/norns   # <drivers/AudioDriver.h>
    ${ACID}                                 # <engine/...>, <sequencer/...>, EngineHost.h
    ${ACID}/platform/desktop                # <android/log.h>
    ${ACID}/platform                        # <link/LinkTimebase.h>
    ${ACID}/platform/web)                   # <jni.h>
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

# Remounts a part while its Cumulus cloud builds.
add_executable(acidulous-cloud-test tests/cloud_race.cpp)
target_include_directories(acidulous-cloud-test PRIVATE src)
target_link_libraries(acidulous-cloud-test acidulous-core)

# Loads, reloads and remounts the sample machines' files.
add_executable(acidulous-sample-test tests/sample_race.cpp)
target_include_directories(acidulous-sample-test PRIVATE src)
target_link_libraries(acidulous-sample-test acidulous-core)

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

    # Core, worker and OSC bridge start and stop, without scsynth.
    add_executable(acidulous-test tests/lifecycle.cpp src/bridge.cpp)
    target_include_directories(acidulous-test PRIVATE src ${LO_INC})
    target_link_libraries(acidulous-test acidulous-core ${LO_LIB})
else()
    message(STATUS "No SuperCollider plugin headers or liblo: building only acidulous-render")
endif()
