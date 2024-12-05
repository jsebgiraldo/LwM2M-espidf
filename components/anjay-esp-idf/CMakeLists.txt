# Copyright 2023-2024 AVSystem <avsystem@avsystem.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

file(GLOB_RECURSE ANJAY_SOURCES
     "deps/anjay/src/*.c"
     "deps/anjay/deps/avs_coap/src/*.c"
     "deps/anjay/deps/avs_commons/src/*.c")

idf_component_register(SRCS
                           ${ANJAY_SOURCES}
                       INCLUDE_DIRS
                           "config"
                           "deps/anjay/include_public"
                           "deps/anjay/deps/avs_coap/include_public"
                           "deps/anjay/deps/avs_commons/include_public"
                       PRIV_INCLUDE_DIRS
                           "deps/anjay/src"
                           "deps/anjay/deps/avs_coap/src"
                           "deps/anjay/deps/avs_commons/src"
                       PRIV_REQUIRES
                           idf::mbedtls
                       REQUIRES
                           esp_driver_uart
                           esp_driver_gpio)

if (CONFIG_ANJAY_ESP_IDF_WITH_BG96_SUPPORT)
    file(GLOB_RECURSE FREERTOS_CELLULAR_LIBRARY_SOURCES
                             "deps/FreeRTOS-Cellular-Interface/source/*.c"
                             "deps/FreeRTOS-Cellular-Interface/modules/bg96/*.c"
                             "deps/FreeRTOS-Cellular-Interface/modules/cellular_platform.c")
    add_library(freertos_cellular_library MODULE ${FREERTOS_CELLULAR_LIBRARY_SOURCES})
    target_include_directories(freertos_cellular_library PRIVATE
                             "deps/FreeRTOS-Cellular-Interface/source/include/private"
                             "deps/FreeRTOS-Cellular-Interface/modules/bg96")
    target_include_directories(freertos_cellular_library PUBLIC
                             "deps/FreeRTOS-Cellular-Interface/source/include"
                             "deps/FreeRTOS-Cellular-Interface/source/include/common"
                             "deps/FreeRTOS-Cellular-Interface/source/logging"
                             "deps/FreeRTOS-Cellular-Interface/source/cellular_network_transport"
                             "deps/FreeRTOS-Cellular-Interface/source/interface"
                             "deps/FreeRTOS-Cellular-Interface/modules")
    # Ignoring warnings in FreeRTOS-Cellular-Interface
    target_compile_options(freertos_cellular_library PRIVATE -Wno-format -Wno-incompatible-pointer-types)
    target_link_libraries(freertos_cellular_library PRIVATE ${COMPONENT_LIB})
endif()

# NOTE: avs_coap contains some assertions that check if allocated memory
# follows alignment requirements. malloc() on ESP-IDF aligns everything
# to 4 bytes, even though alignof(max_align_t) == alignof(int64_t)
# == alignof(long double) == 8. This is just GCC being overly cautious,
# the maximum hardware-imposed alignment on ESP32 is actually 4 bytes.
# We disable assertions within anjay to work around that.
target_compile_definitions(${COMPONENT_TARGET} PRIVATE "NDEBUG")

# maybe-uninitialized warning not treated as error to enable debug optimization
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=maybe-uninitialized)
