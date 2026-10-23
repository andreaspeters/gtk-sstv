cmake_minimum_required(VERSION 3.16)
project(sstv VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(GNUInstallDirs)
include(CTest)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0 gdk-pixbuf-2.0)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(FFTW3 REQUIRED IMPORTED_TARGET fftw3)
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources)
if(NOT GLIB_COMPILE_RESOURCES)
    message(FATAL_ERROR "glib-compile-resources is required to embed the UI and image assets")
endif()

set(SSTV_UI_FILES
    src/aboutdialog.ui
    src/prefs.ui
    src/slowrx.ui
)
set(SSTV_RUNTIME_ASSETS
    assets/sstv.png
    assets/testbild.png
)
set(SSTV_RESOURCE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/sstv-resources.c")
add_custom_command(
    OUTPUT "${SSTV_RESOURCE_SOURCE}"
    COMMAND "${GLIB_COMPILE_RESOURCES}"
            --generate-source
            "--sourcedir=${CMAKE_CURRENT_SOURCE_DIR}"
            "--target=${SSTV_RESOURCE_SOURCE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/sstv.gresource.xml"
    DEPENDS
        src/sstv.gresource.xml
        ${SSTV_UI_FILES}
        ${SSTV_RUNTIME_ASSETS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM
)

set(SSTV_APP_SOURCES
    src/adifudp.c
    src/common.c
    src/compose.c
    src/encoder.c
    src/fsk.c
    src/gui.c
    src/modespec.c
    src/pcm.c
    src/slowrx.c
    src/sync.c
    src/txaudio.c
    src/txdialog.c
    src/video.c
    src/vis.c
)

add_library(sstv_resources OBJECT "${SSTV_RESOURCE_SOURCE}")
target_link_libraries(sstv_resources PRIVATE PkgConfig::GLIB2)

add_executable(sstv ${SSTV_APP_SOURCES})
target_include_directories(sstv PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(sstv PRIVATE
    sstv_resources
    PkgConfig::GTK4
    PkgConfig::GLIB2
    PkgConfig::ALSA
    PkgConfig::FFTW3
    Threads::Threads
    m
)

if(BUILD_TESTING)
    add_executable(test_adifudp src/test_adifudp.c src/adifudp.c)
    target_include_directories(test_adifudp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_adifudp PRIVATE PkgConfig::GLIB2)
    add_test(NAME adifudp COMMAND test_adifudp)

    add_executable(test_compose src/test_compose.c src/compose.c)
    target_include_directories(test_compose PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_compose PRIVATE sstv_resources PkgConfig::GTK4 PkgConfig::GLIB2 m)
    add_test(NAME compose COMMAND test_compose)
    set_tests_properties(compose PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(test_encoder src/test_encoder.c src/encoder.c)
    target_include_directories(test_encoder PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_encoder PRIVATE PkgConfig::GTK4 PkgConfig::GLIB2 m)
    add_test(NAME encoder COMMAND test_encoder)

    add_executable(test_txaudio src/test_txaudio.c src/txaudio.c)
    target_include_directories(test_txaudio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(test_txaudio PRIVATE PkgConfig::GLIB2 PkgConfig::ALSA)
    add_test(NAME txaudio COMMAND test_txaudio)
endif()

install(TARGETS sstv RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES assets/sstv.png
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps")
install(FILES assets/org.sstv.SSTV.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
