/*
 * Copyright 2021-2024 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>

#include <esp_mac.h>

#include <avsystem/commons/avs_utils.h>

#include "utils.h"

int get_device_id(device_id_t *out_id) {
    memset(out_id->value, 0, sizeof(out_id->value));

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) != ESP_OK) {
        return -1;
    }

    return avs_hexlify(out_id->value, sizeof(out_id->value), NULL, mac,
                       sizeof(mac));
}

int8_t datahex(char* string, uint8_t *data, int8_t len) 
{
    if(string == NULL) 
       return -1;

    // Count colons
    int colons = 0;
    size_t index = 0;
    for (index = 0, colons=0; string[index] > 0; index++)
        if(string[index] == ':')
          colons++;

    size_t slength = strlen(string);

    if( ((slength-colons) % 2) != 0) // must be even
       return -1;

    if( (slength - colons)/2 > len)
      return -1;

    memset(data, 0, len);

    index = 0;
    size_t dindex = 0;
    while (index < slength) {
        char c = string[index];
        int value = 0;
        if(c >= '0' && c <= '9')
          value = (c - '0');
        else if (c >= 'A' && c <= 'F') 
          value = (10 + (c - 'A'));
        else if (c >= 'a' && c <= 'f')
          value = (10 + (c - 'a'));
        else if (c == ':') {
            index++;
            continue;
        }
        else {
          return -1;
        }

        data[(dindex/2)] += value << (((dindex + 1) % 2) * 4);

        index++;
        dindex++;
    }

    return 1+dindex;
}