
#include <avsystem/commons/avs_log.h>
#include <avsystem/commons/avs_sched.h>

#include <anjay/anjay.h>
#include <anjay/attr_storage.h>
#include <anjay/core.h>
#include <anjay/security.h>
#include <anjay/server.h>


#include "firmware_update.h"

#include "objects/objects.h"


#define MAIN_PREFERRED_TRANSPORT "U"

#define CONFIG_ANJAY_CLIENT_ENDPOINT_NAME "esp-1053866"
#define CONFIG_ANJAY_CLIENT_SERVER_URI "coaps://eu.iot.avsystem.cloud:5684"
#define CONFIG_ANJAY_CLIENT_PSK_KEY "1234"
#define CONFIG_ANJAY_CLIENT_PSK_IDENTITY "esp-1053866"

static char PSK[ANJAY_MAX_SECRET_KEY_SIZE];
static char IDENTITY[ANJAY_MAX_PK_OR_IDENTITY_SIZE];

static char SERVER_URI[ANJAY_MAX_PK_OR_IDENTITY_SIZE];
static char ENDPOINT_NAME[ANJAY_MAX_PK_OR_IDENTITY_SIZE];

static const anjay_dm_object_def_t **DEVICE_OBJ;

static anjay_t *anjay;
static avs_sched_handle_t connection_status_job_handle;


void anjay_init(void);

void anjay_task(void *pvParameters);