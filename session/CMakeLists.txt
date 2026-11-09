project(lingmo-session)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED 17)

set(TARGET lingmo-session)

set(SOURCES
        application.cpp
        main.cpp
        process.cpp
        processmanager.cpp
        networkproxymanager.cpp

        powermanager/power.cpp
        powermanager/powerproviders.cpp

        daemon-helper.cpp
        daemon-helper.h

        waylandstart.cpp
        waylandstart.h
)

qt6_add_dbus_adaptor(DBUS_SOURCES
                     com.lingmo.Session.xml
                     application.h Application
                     sessionadaptor SessionAdaptor)
set_source_files_properties(${DBUS_SOURCES} PROPERTIES SKIP_AUTOGEN ON)

find_package(KF6WindowSystem)
find_package(KF6Config REQUIRED)
find_package(Threads)

add_executable(${TARGET} ${SOURCES} ${DBUS_SOURCES})
target_link_libraries(${TARGET}
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Quick
        Qt6::DBus
        Qt6::GuiPrivate
        KF6::WindowSystem
        KF6::ConfigCore
        ${CMAKE_THREAD_LIBS_INIT}
)

install(TARGETS ${TARGET} DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS lingmo-reload DESTINATION ${CMAKE_INSTALL_BINDIR})
# Never rename these: SDDM remembers the last session by file name (state.conf)
install(FILES lingmo-xsession.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/xsessions/)
install(FILES lingmo-wayland.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/wayland-sessions/)

# Wayland session: GPU check and fallback to X11, run by SDDM before the greeter
install(PROGRAMS wayland-fallback/lingmo-wayland-check wayland-fallback/sddm-xsetup
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/lingmo)
install(FILES wayland-fallback/sddm-lingmo-wayland.conf
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/sddm/sddm.conf.d RENAME lingmo-wayland.conf)
