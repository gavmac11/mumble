# Copyright The Mumble Developers. All rights reserved.
# BSD-style license: see LICENSE at the source root.
include_guard(GLOBAL)

function(mumble_repair_qt_macos_accessibility)
    if(NOT APPLE OR NOT static OR NOT TARGET Qt6::QCocoaIntegrationPlugin)
        return()
    endif()
    option(qt-cocoa-accessibility-repair "Repair model-owned Qt 6.10.0 Cocoa accessibility elements" ON)
    if(NOT qt-cocoa-accessibility-repair OR NOT Qt6_VERSION VERSION_EQUAL "6.10.0")
        return()
    endif()

    find_package(Qt6 6.10.0 EXACT REQUIRED COMPONENTS CorePrivate GuiPrivate)
    include(FetchContent)
    FetchContent_Declare(mumble_qt_cocoa_source
        URL https://codeload.github.com/qt/qtbase/tar.gz/5a8637e4516bc48a0b3f4b5ec3b18618b92e7222
        URL_HASH SHA256=69c4fe6b0f8b21e555c46ff0541d4f47e0a25bb01bfd81aeb2b10d447b01cb19
        SOURCE_SUBDIR _mumble_source_only
    )
    FetchContent_MakeAvailable(mumble_qt_cocoa_source)
    set(MUMBLE_QT_COCOA_SOURCE_DIR "${mumble_qt_cocoa_source_SOURCE_DIR}/src/plugins/platforms/cocoa")
    set(accessibility_source "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaaccessibilityelement.mm")
    file(SHA256 "${accessibility_source}" source_digest)
    if(source_digest STREQUAL "825577252232647c6dc01b57c808c1c6518036ec0eab1315038070aba28f295a")
        find_program(MUMBLE_QT_PATCH_EXECUTABLE patch REQUIRED)
        execute_process(
            COMMAND "${MUMBLE_QT_PATCH_EXECUTABLE}" --batch --fuzz=0 -p1
                -i "${CMAKE_SOURCE_DIR}/docs/stability/qt-accessibility/qt-parent-owned-accessibility.patch"
            WORKING_DIRECTORY "${mumble_qt_cocoa_source_SOURCE_DIR}"
            RESULT_VARIABLE patch_result
            OUTPUT_VARIABLE patch_output
            ERROR_VARIABLE patch_error
        )
        if(NOT patch_result EQUAL 0)
            message(FATAL_ERROR "Qt Cocoa accessibility patch failed: ${patch_output}${patch_error}")
        endif()
        file(SHA256 "${accessibility_source}" source_digest)
    endif()
    if(NOT source_digest STREQUAL "8591bb3f34d20aa23a681352336b9e9fb8b379e07a56198d1faaff12e0ec2311")
        message(FATAL_ERROR "Unexpected Qt Cocoa accessibility source; refusing an unverified repair")
    endif()

    set(repair_build_dir "${CMAKE_CURRENT_BINARY_DIR}/qt-cocoa-repair")
    add_subdirectory("${CMAKE_SOURCE_DIR}/cmake/qt-cocoa-repair" "${repair_build_dir}")
    set(repaired_archive "${repair_build_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}mumble_qt_cocoa${CMAKE_STATIC_LIBRARY_SUFFIX}")
    # Preserve Qt's plugin import/interface metadata; replace only its archive
    # in this build. The installed/cached dependency archive is never modified.
    get_target_property(qt_plugin_configs Qt6::QCocoaIntegrationPlugin IMPORTED_CONFIGURATIONS)
    foreach(config IN LISTS qt_plugin_configs)
        string(TOUPPER "${config}" config)
        set_property(TARGET Qt6::QCocoaIntegrationPlugin PROPERTY "IMPORTED_LOCATION_${config}" "${repaired_archive}")
    endforeach()
    set_property(TARGET Qt6::QCocoaIntegrationPlugin PROPERTY IMPORTED_LOCATION "${repaired_archive}")
    add_dependencies(Qt6::QCocoaIntegrationPlugin mumble_qt_cocoa)
    message(STATUS "Rebuilding Qt 6.10.0 Cocoa plugin with verified model-owned accessibility repair")
endfunction()
