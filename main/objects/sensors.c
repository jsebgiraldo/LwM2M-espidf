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

#include <assert.h>
#include <stdbool.h>

#include <anjay/anjay.h>
#include <anjay/ipso_objects.h>
#include <anjay/ipso_objects_v2.h>
#include <anjay/security.h>
#include <anjay/server.h>

#include <avsystem/commons/avs_log.h>
#include <avsystem/commons/avs_sched.h>

#include "objects/objects.h"
#include "sdkconfig.h"

#define TEMPERATURE_OBJ_OID 3303

typedef struct {
    const char *name;
    const char *unit;
    anjay_oid_t oid;
    double data;
    int (*read_data)(void);
    void (*get_data)(double *sensor_data);
} basic_sensor_context_t;

static double temperature_sensor_data;
int temperature_read_data(void) {
    uint8_t temp[2];
    if (1) {
        temperature_sensor_data = 22;
        return 0;
    } else {
        return -1;
    }
}

void temperature_get_data(double *sensor_data) {
    *sensor_data = temperature_sensor_data;
}

static basic_sensor_context_t BASIC_SENSORS_DEF[] = {
    {
        .name = "Temperature sensor",
        .unit = "Cel",
        .oid = TEMPERATURE_OBJ_OID,
        .read_data = temperature_read_data,
        .get_data = temperature_get_data,
    },
    {
        .name = "Humidity sensor",
        .unit = "Cel",
        .oid = 3304,
        .read_data = temperature_read_data,
        .get_data = temperature_get_data,
    },
};

int basic_sensor_get_value(anjay_iid_t iid, void *_ctx, double *value) {
    basic_sensor_context_t *ctx = (basic_sensor_context_t *) _ctx;

    assert(ctx->read_data);
    assert(ctx->get_data);
    assert(value);

    if (!ctx->read_data()) {
        ctx->get_data(&ctx->data);
        *value = ctx->data;
        return 0;
    } else {
        return -1;
    }
}

void sensors_install(anjay_t *anjay) {
#if CONFIG_ANJAY_CLIENT_BOARD_M5STICKC_PLUS
    if (mpu6886_device_init()) {
        avs_log(ipso_object,
                WARNING,
                "Driver for MPU6886 could not be initialized!");
        return;
    }
#endif

    for (int i = 0; i < (int) AVS_ARRAY_SIZE(BASIC_SENSORS_DEF); i++) {
        basic_sensor_context_t *ctx = &BASIC_SENSORS_DEF[i];

        if (anjay_ipso_basic_sensor_install(anjay, ctx->oid, 1)) {
            avs_log(ipso_object,
                    WARNING,
                    "Object: %s could not be installed",
                    ctx->name);
            continue;
        }

        if (anjay_ipso_basic_sensor_instance_add(
                    anjay,
                    ctx->oid,
                    0,
                    (anjay_ipso_basic_sensor_impl_t) {
                        .unit = ctx->unit,
                        .user_context = ctx,
                        .min_range_value = NAN,
                        .max_range_value = NAN,
                        .get_value = basic_sensor_get_value
                    })) {
            avs_log(ipso_object,
                    WARNING,
                    "Instance of %s object could not be added",
                    ctx->name);
        }
    }
}

void sensors_update(anjay_t *anjay) {
    for (int i = 0; i < (int) AVS_ARRAY_SIZE(BASIC_SENSORS_DEF); i++) {
        anjay_ipso_basic_sensor_update(anjay, BASIC_SENSORS_DEF[i].oid, 0);
    }
}

void sensors_release(void) {
#if CONFIG_ANJAY_CLIENT_BOARD_M5STICKC_PLUS
    mpu6886_driver_release();
#endif // CONFIG_ANJAY_CLIENT_BOARD_M5STICKC_PLUS
}
