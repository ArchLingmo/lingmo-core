find_package(X11)
find_package(KF6Config REQUIRED)

add_executable(cupdatecursor
  main.cpp
)
target_link_libraries(cupdatecursor
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    KF6::ConfigCore
    ${X11_LIBRARIES}
    X11::X11
    X11::Xi
    X11::Xcursor
)

install(TARGETS cupdatecursor DESTINATION ${CMAKE_INSTALL_BINDIR})
