add_library(ps2kbd STATIC
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_wrapper.cpp
)

target_link_libraries(ps2kbd PRIVATE hardware_pio hardware_clocks)

# KBD_CLOCK_PIN selects which PIO program is generated for the right pin.
if(BOARD_VARIANT STREQUAL "M2")
    target_compile_definitions(ps2kbd PRIVATE BOARD_M2 KBD_CLOCK_PIN=2)
else()
    target_compile_definitions(ps2kbd PRIVATE BOARD_M1 KBD_CLOCK_PIN=0)
endif()

target_include_directories(ps2kbd PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_include_directories(ps2kbd PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

pico_generate_pio_header(ps2kbd
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr.pio
)
pico_generate_pio_header(ps2kbd
    ${CMAKE_CURRENT_LIST_DIR}/ps2kbd_mrmltr2.pio
)
