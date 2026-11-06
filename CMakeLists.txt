cmake_minimum_required(VERSION 3.16)
project(dsi-photoConverter VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_executable(dsi-photoConverter src/dsi_photo_converter.c)
target_link_libraries(dsi-photoConverter PRIVATE JPEG::JPEG PNG::PNG OpenSSL::Crypto)
if(MSVC)
    target_compile_options(dsi-photoConverter PRIVATE /W4)
else()
    target_compile_options(dsi-photoConverter PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS dsi-photoConverter RUNTIME DESTINATION bin)
install(FILES README.md LICENSE THIRD_PARTY.md DESTINATION .)
set(CPACK_PACKAGE_NAME "dsi-photoConverter")
set(CPACK_PACKAGE_VENDOR "community")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "JPEG/PNG converter and SD album importer for Nintendo DSi Camera")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "ZIP")
include(CPack)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME create_signed_jpeg
        COMMAND $<TARGET_FILE:dsi-photoConverter>
            --output "${CMAKE_CURRENT_BINARY_DIR}/test_HNI_0001.JPG"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/pattern.png")
    add_test(NAME verify_signed_jpeg
        COMMAND $<TARGET_FILE:dsi-photoConverter>
            --verify "${CMAKE_CURRENT_BINARY_DIR}/test_HNI_0001.JPG")
    set_tests_properties(verify_signed_jpeg PROPERTIES DEPENDS create_signed_jpeg)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME batch_and_pit_import
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_batch.py"
                $<TARGET_FILE:dsi-photoConverter> "${CMAKE_CURRENT_SOURCE_DIR}/tests/pattern.png")
    endif()
endif()
