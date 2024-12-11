set(sources
     "main.c"
     "utils.c"
     "anjay_config.c"
     "objects/device.c"
     "objects/light_control.c"
     "objects/push_button.c"
     "objects/sensors.c"
     "firmware_update.c")

set(dirs
     ".")

idf_component_register(SRCS ${sources}
                       INCLUDE_DIRS ${dirs})
