# The game UI: input, draw lists, interaction and fonts, and the original's interface built from
# its layouts (the window system, the controls, the keys). No GPU code
# (the client's UI pass draws the lists), so the tests build it without the client.
add_library(sr_ui STATIC
    src/ClassLayouts.cpp
    src/ControlsBasic.cpp
    src/ControlsFrames.cpp
    src/ControlsSlots.cpp
    src/ControlsText.cpp
    src/ControlsValues.cpp
    src/Context.cpp
    src/DrawList.cpp
    src/Font.cpp
    src/Hotkeys.cpp
    src/Input.cpp
    src/Interface.cpp
    src/Painter.cpp
    src/Widget.cpp
)
add_library(sr::ui ALIAS sr_ui)

target_include_directories(sr_ui PUBLIC include)
target_link_libraries(sr_ui PUBLIC sr::core sr::assets PRIVATE stb freetype sr_compile_options)
