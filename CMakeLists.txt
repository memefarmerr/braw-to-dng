cmake_minimum_required(VERSION 3.16)
project(braw_to_dng CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Root of the Blackmagic RAW SDK, i.e. the folder that contains Mac/ and Win/.
if(WIN32)
    if(DEFINED ENV{ProgramFiles\(x86\)})
        set(_braw_sdk_default "$ENV{ProgramFiles\(x86\)}/Blackmagic Design/Blackmagic RAW/Blackmagic RAW SDK")
    else()
        set(_braw_sdk_default "C:/Program Files (x86)/Blackmagic Design/Blackmagic RAW/Blackmagic RAW SDK")
    endif()
else()
    set(_braw_sdk_default "/Applications/Blackmagic RAW/Blackmagic RAW SDK")
endif()

set(BRAW_SDK_ROOT "${_braw_sdk_default}" CACHE PATH "Blackmagic RAW SDK root directory")

if(WIN32)
    set(BRAW_SDK_PLATFORM "Win")
else()
    set(BRAW_SDK_PLATFORM "Mac")
endif()

set(BRAW_SDK_INCLUDE "${BRAW_SDK_ROOT}/${BRAW_SDK_PLATFORM}/Include")
set(BRAW_SDK_LIBRARIES "${BRAW_SDK_ROOT}/${BRAW_SDK_PLATFORM}/Libraries")

if(NOT EXISTS "${BRAW_SDK_INCLUDE}")
    message(WARNING "Blackmagic RAW SDK headers not found in '${BRAW_SDK_INCLUDE}'. "
                    "Configure with -DBRAW_SDK_ROOT=<sdk root>.")
endif()

add_executable(braw_to_dng
    main.cpp
    src/braw_decoder.cpp
    src/deflate.cpp
    src/image_writer.cpp
    "${BRAW_SDK_INCLUDE}/BlackmagicRawAPIDispatch.cpp"
)

# win/ provides the C++ header for the SDK's IDL; see tools/generate_win_header.py.
if(WIN32)
    target_include_directories(braw_to_dng PRIVATE win "${BRAW_SDK_INCLUDE}")
    target_link_libraries(braw_to_dng PRIVATE ole32 oleaut32)
else()
    target_include_directories(braw_to_dng PRIVATE "${BRAW_SDK_INCLUDE}")
    target_link_libraries(braw_to_dng PRIVATE
        "-framework CoreFoundation"
        "-framework CoreServices"
        "-framework CoreMedia"
        "-framework CoreGraphics"
        "${BRAW_SDK_LIBRARIES}/BlackmagicRawAPI.framework/BlackmagicRawAPI")
    set_target_properties(braw_to_dng PROPERTIES LINK_FLAGS "-Wl,-F${BRAW_SDK_LIBRARIES}")
endif()

target_include_directories(braw_to_dng PRIVATE src)

# The SDK's own dispatch source trips -Wcast-function-type on GCC/Clang.
if(NOT MSVC)
    set_source_files_properties("${BRAW_SDK_INCLUDE}/BlackmagicRawAPIDispatch.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wno-cast-function-type")
endif()

if(MSVC)
    target_compile_options(braw_to_dng PRIVATE /W4)
else()
    target_compile_options(braw_to_dng PRIVATE -Wall -Wextra)
    if(WIN32)
        # Keep the executable free of MinGW runtime DLL dependencies.
        target_link_options(braw_to_dng PRIVATE -static)
    endif()
endif()
