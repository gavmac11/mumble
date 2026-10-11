# Copyright The Mumble Developers. All rights reserved.
# BSD-style license: see LICENSE at the source root.
# This target compiles pinned upstream Qt sources, with the patch recorded in
# docs/stability/qt-accessibility. Qt source files retain their upstream license.

# Vendor Qt sources use Qt's compiler policy rather than Mumble's warnings.
set_directory_properties(PROPERTIES COMPILE_OPTIONS "")
# Match upstream Cocoa's deployment requirement. The client minimum stays 15.
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(cocoa_sources
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/main.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaapplication.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaapplication.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaapplicationdelegate.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaapplicationdelegate.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoabackingstore.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoabackingstore.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaclipboard.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaclipboard.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoacursor.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoacursor.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoadrag.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoadrag.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaeventdispatcher.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaeventdispatcher.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoahelpers.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoahelpers.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoainputcontext.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoainputcontext.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaintegration.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaintegration.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaintrospection.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaintrospection.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamenu.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamenu.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamenubar.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamenubar.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamenuitem.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamenuitem.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamenuloader.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamenuloader.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamimetypes.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamimetypes.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoanativeinterface.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoanativeinterface.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoansmenu.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoansmenu.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoascreen.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoascreen.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaservices.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaservices.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoasystemtrayicon.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoasystemtrayicon.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoatheme.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoatheme.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoawindow.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoawindow.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoawindowmanager.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoawindowmanager.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qiosurfacegraphicsbuffer.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qiosurfacegraphicsbuffer.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qmacclipboard.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qmacclipboard.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qmultitouch_mac.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qmultitouch_mac_p.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qnsview.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qnsview.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qnswindow.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qnswindow.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qnswindowdelegate.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qnswindowdelegate.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoacolordialoghelper.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoacolordialoghelper.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoafiledialoghelper.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoafiledialoghelper.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoafontdialoghelper.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoafontdialoghelper.mm"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamessagedialog.h"
    "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoamessagedialog.mm"
)
foreach(feature IN ITEMS opengl vulkan accessibility sessionmanager)
    if(QT_FEATURE_${feature})
        if(feature STREQUAL "opengl")
            list(APPEND cocoa_sources "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaglcontext.mm")
        elseif(feature STREQUAL "vulkan")
            list(APPEND cocoa_sources "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoavulkaninstance.mm")
        elseif(feature STREQUAL "accessibility")
            list(APPEND cocoa_sources "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaaccessibility.mm"
                "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoaaccessibilityelement.mm")
        elseif(feature STREQUAL "sessionmanager")
            list(APPEND cocoa_sources "${MUMBLE_QT_COCOA_SOURCE_DIR}/qcocoasessionmanager.cpp")
        endif()
    endif()
endforeach()
if(NOT QT_FEATURE_accessibility)
    message(FATAL_ERROR "Qt Cocoa repair requires an accessibility-enabled Qt build")
endif()
add_library(mumble_qt_cocoa STATIC ${cocoa_sources})
set_target_properties(mumble_qt_cocoa PROPERTIES AUTOMOC ON UNITY_BUILD OFF
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_property(TARGET mumble_qt_cocoa PROPERTY "ARCHIVE_OUTPUT_DIRECTORY_${config}" "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()
target_compile_definitions(mumble_qt_cocoa PRIVATE QT_NO_FOREACH QT_PLUGIN QT_STATICPLUGIN)
target_include_directories(mumble_qt_cocoa PRIVATE "${MUMBLE_QT_COCOA_SOURCE_DIR}")
target_link_libraries(mumble_qt_cocoa PRIVATE Qt6::Core Qt6::CorePrivate Qt6::Gui Qt6::GuiPrivate)
foreach(framework IN ITEMS Foundation AppKit Carbon CoreServices CoreVideo IOKit IOSurface Metal QuartzCore UniformTypeIdentifiers)
    find_library(MUMBLE_QT_${framework} ${framework} REQUIRED)
    target_link_libraries(mumble_qt_cocoa PRIVATE "${MUMBLE_QT_${framework}}")
endforeach()
if(QT_FEATURE_opengl)
    find_library(MUMBLE_QT_OpenGL OpenGL REQUIRED)
    target_link_libraries(mumble_qt_cocoa PRIVATE "${MUMBLE_QT_OpenGL}")
endif()
qt_add_resources(mumble_qt_cocoa qcocoaresources
    PREFIX "/qt-project.org/mac/cursors"
    BASE "${MUMBLE_QT_COCOA_SOURCE_DIR}"
    FILES "${MUMBLE_QT_COCOA_SOURCE_DIR}/images/sizeallcursor.png"
        "${MUMBLE_QT_COCOA_SOURCE_DIR}/images/spincursor.png"
        "${MUMBLE_QT_COCOA_SOURCE_DIR}/images/waitcursor.png")
