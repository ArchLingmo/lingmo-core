project(lingmo-userdirs)

# Rename/merge engine: Qt Core only, shared by the dialog and the tests
add_library(lingmo-userdirs-engine STATIC
    engine/userdirs.h
    engine/names.cpp
    engine/config.cpp
    engine/fsops.cpp
    engine/rewriter.cpp
    engine/planner.cpp
    engine/executor.cpp
)
target_include_directories(lingmo-userdirs-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lingmo-userdirs-engine PUBLIC Qt6::Core)

add_executable(lingmo-userdirs
    main.cpp
    backend.cpp
    backend.h
    qml.qrc
)

file(GLOB TS_FILES translations/*.ts)
foreach(filepath ${TS_FILES})
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" filename ${filepath})
    list(APPEND userdirs_ts_files ${filename})
endforeach()
qt_add_translation(USERDIRS_QM_FILES ${userdirs_ts_files})
add_custom_target(userdirs-translations DEPENDS ${USERDIRS_QM_FILES} SOURCES ${userdirs_ts_files})
add_dependencies(lingmo-userdirs userdirs-translations)

target_link_libraries(lingmo-userdirs
    lingmo-userdirs-engine
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::DBus
)

install(TARGETS lingmo-userdirs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${USERDIRS_QM_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/lingmo-userdirs/translations)
# Login check: asks once per locale (the session skips xdg-user-dirs-gtk's prompt)
install(FILES lingmo-userdirs.desktop DESTINATION /etc/xdg/autostart)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(tst_userdirs tests/tst_userdirs.cpp)
    target_link_libraries(tst_userdirs lingmo-userdirs-engine Qt6::Test)
    add_test(NAME tst_userdirs COMMAND tst_userdirs)
endif()
