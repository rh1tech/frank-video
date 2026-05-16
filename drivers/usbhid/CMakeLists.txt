# USB HID host driver. The library exists as an INTERFACE target either
# way so the top-level CMakeLists can link it unconditionally; sources
# and TinyUSB linkage are only added when USB_HID_ENABLED is set.

add_library(usbhid INTERFACE)

if(USB_HID_ENABLED)
    message(STATUS "USB HID Host enabled")
    message(STATUS "  USB CDC stdio is DISABLED in this build (UART only).")

    target_sources(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/hid_app.c
        ${CMAKE_CURRENT_LIST_DIR}/usbhid_wrapper.c
    )

    # tusb_config.h lives here; only expose it when host mode is active so
    # the default device-mode build can't accidentally pick it up.
    target_include_directories(usbhid INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
    )

    target_compile_definitions(usbhid INTERFACE
        USB_HID_ENABLED=1
    )

    target_link_libraries(usbhid INTERFACE
        tinyusb_host
        tinyusb_board
    )
else()
    message(STATUS "USB HID Host disabled (use -DUSB_HID_ENABLED=ON to enable)")
endif()
