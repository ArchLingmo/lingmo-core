project(lingmo-settings-daemon)
set(TARGET lingmo-settings-daemon)

find_package(ECM REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

find_package(KF6Config REQUIRED)

# X11 backends (libinput/synaptics properties, setxkbmap and XKB state, RandR
# gamma for the night light) are optional: without them the daemon only has the
# Wayland ones (KWin over D-Bus, kxkbrc, kwinrc [NightColor]).
# The same binary picks its backend at run time.
option(WITH_X11 "Build the X11 backends" ON)
if(WITH_X11)
    find_package(X11)
    find_package(X11_XCB)
    find_package(XCB COMPONENTS XCB RANDR XKB RECORD)
    pkg_check_modules(XORGLIBINPUT xorg-libinput IMPORTED_TARGET)
    pkg_check_modules(XORGSERVER xorg-server IMPORTED_TARGET)
    pkg_check_modules(SYNAPTICS xorg-synaptics IMPORTED_TARGET GLOBAL)
    if(X11_FOUND AND X11_Xi_FOUND AND X11_XCB_FOUND AND XCB_FOUND AND XCB_RANDR_FOUND AND XCB_XKB_FOUND AND XCB_RECORD_FOUND
       AND XORGLIBINPUT_FOUND AND XORGSERVER_FOUND AND SYNAPTICS_FOUND)
        set(HAVE_X11 ON)
    else()
        message(WARNING "X11 development files missing: lingmo-settings-daemon is built without its X11 backends")
    endif()
endif()

file (GLOB_RECURSE SRCS
  "*.cpp"
  "*.h"
  "theme/*.h"
  "theme/*.cpp"
  "brightness/*.h"
  "brightness/*.cpp"
  "brightness/*.c"
  "battery/*.cpp"
  "battery/*.h"
  "language/*.cpp"
  "language/*.h"
  "dock/*.cpp"
  "dock/*.h"
  "mouse/*.h"
  "mouse/*.cpp"
  "touchpad/*.h"
  "touchpad/*.cpp"
  "touchpad/x11/*.h"
  "touchpad/x11/*.cpp"
  "nightlight/*.h"
  "nightlight/*.cpp"
  "keyboard/*.h"
  "keyboard/*.cpp"
  "input/*.h"
  "input/*.cpp"
)
if(NOT HAVE_X11)
    list(FILTER SRCS EXCLUDE REGEX "/touchpad/x11/|/mouse/x11libinputdummydevice")
endif()

set(SOURCES ${SRCS})
set(HEADERS "")
set(FORMS "")
set(RESOURCES "")

qt6_add_dbus_adaptor(DBUS_SOURCES
                     brightness/com.lingmo.Brightness.xml
                     brightness/brightnessmanager.h BrightnessManager)
qt6_add_dbus_adaptor(DBUS_SOURCES
                     theme/com.lingmo.Theme.xml
                     theme/thememanager.h ThemeManager)
qt6_add_dbus_adaptor(DBUS_SOURCES
                     battery/com.lingmo.PrimaryBattery.xml
                     battery/battery.h Battery)
qt6_add_dbus_adaptor(DBUS_SOURCES
                     language/com.lingmo.Language.xml
                     language/language.h Language)
qt6_add_dbus_adaptor(DBUS_SOURCES
                     dock/com.lingmo.Dock.xml
                     dock/dock.h Dock)
qt6_add_dbus_adaptor(DBUS_SOURCES
                     mouse/com.lingmo.Mouse.xml
                     mouse/mousemanager.h Mouse)
qt6_add_dbus_adaptor(DBUS_SOURCES
                     touchpad/com.lingmo.Touchpad.xml
                     touchpad/touchpadmanager.h TouchpadManager)
qt6_add_dbus_adaptor(DBUS_SOURCES
                     nightlight/com.lingmo.NightLight.xml
                     nightlight/nightlight.h NightLight)
qt6_add_dbus_adaptor(DBUS_SOURCES
                     keyboard/com.lingmo.Keyboard.xml
                     keyboard/keyboard.h Keyboard)
set_source_files_properties(${DBUS_SOURCES} PROPERTIES SKIP_AUTOGEN ON)

add_executable(${TARGET} ${SOURCES} ${DBUS_SOURCES} ${HEADERS} ${FORMS} ${RESOURCES})

file(GLOB TS_FILES translations/*.ts)
foreach(filepath ${TS_FILES})
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" filename ${filepath})
    list(APPEND ts_files_replaced ${filename})
endforeach()
qt_add_translation(QM_FILES ${ts_files_replaced})

target_link_libraries(${TARGET}
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::DBus
        Qt6::GuiPrivate
        Qt6::Xml
        KF6::ConfigCore
)
if(HAVE_X11)
    target_compile_definitions(${TARGET} PRIVATE HAVE_X11)
    target_link_libraries(${TARGET}
        ${X11_LIBRARIES}
        X11::X11
        X11::Xi
        X11::XCB
        ${XCB_LIBRARIES}
        XCB::RANDR
        XCB::XKB
        PkgConfig::XORGLIBINPUT
        PkgConfig::XORGSERVER
        PkgConfig::SYNAPTICS
    )
endif()

add_custom_target(translations DEPENDS ${QM_FILES} SOURCES ${ts_files_replaced})
add_dependencies(${TARGET} translations)

install(TARGETS ${TARGET} DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${QM_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/${TARGET}/translations)
