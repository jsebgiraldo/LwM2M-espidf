#include "anjay_config.h"
#include <openthread/thread.h>
#include "esp_openthread.h"

static int read_anjay_config(void) {
    int err = 0;

        avs_log(tutorial, WARNING,"Reading from NVS has failed, attempt with Kconfig");
        snprintf(ENDPOINT_NAME, sizeof(ENDPOINT_NAME), "%s",CONFIG_ANJAY_CLIENT_ENDPOINT_NAME);
        snprintf(SERVER_URI, sizeof(SERVER_URI), "%s",CONFIG_ANJAY_CLIENT_SERVER_URI);
        snprintf(PSK, sizeof(PSK), "%s", CONFIG_ANJAY_CLIENT_PSK_KEY);
        snprintf(IDENTITY, sizeof(IDENTITY), "%s",CONFIG_ANJAY_CLIENT_PSK_IDENTITY);
        err = -1;
    return err;
}

static void update_objects_job(avs_sched_t *sched, const void *anjay_ptr) {
    anjay_t *anjay = *(anjay_t *const *) anjay_ptr;

    device_object_update(anjay, DEVICE_OBJ);
    sensors_update(anjay);

    AVS_SCHED_DELAYED(sched, &sensors_job_handle,
                    avs_time_duration_from_scalar(1, AVS_TIME_S),
                    &update_objects_job, &anjay, sizeof(anjay));
}

static void update_connection_status_job(avs_sched_t *sched,
                                         const void *anjay_ptr) {
    anjay_t *anjay = *(anjay_t *const *) anjay_ptr;

    //anjay_transport_exit_offline(anjay, ANJAY_TRANSPORT_SET_IP);

    AVS_SCHED_DELAYED(sched, &connection_status_job_handle,
                      avs_time_duration_from_scalar(1, AVS_TIME_S),
                      update_connection_status_job, &anjay, sizeof(anjay));
}



static int setup_security_object(anjay_t *anjay) {
    if (anjay_security_object_install(anjay)) {
        return -1;
    }

    anjay_security_instance_t security_instance = {
        .ssid = 1,
        .server_uri = SERVER_URI,
        .security_mode = ANJAY_SECURITY_PSK,
        .public_cert_or_psk_identity = (const uint8_t *) IDENTITY,
        .public_cert_or_psk_identity_size = strlen(IDENTITY),
        .private_cert_or_psk_key = (const uint8_t *) PSK,
        .private_cert_or_psk_key_size = strlen(PSK)
    };

    // Anjay will assign Instance ID automatically
    anjay_iid_t security_instance_id = ANJAY_ID_INVALID;
    if (anjay_security_object_add_instance(anjay, &security_instance,
                                           &security_instance_id)) {
        return -1;
    }

    return 0;
}

static int setup_server_object(anjay_t *anjay) {
    if (anjay_server_object_install(anjay)) {
        return -1;
    }

    const anjay_server_instance_t server_instance = {
        // Server Short ID
        .ssid = 1,
        // Client will send Update message often than every 60 seconds
        .lifetime = 60,
        // Disable Default Minimum Period resource
        .default_min_period = -1,
        // Disable Default Maximum Period resource
        .default_max_period = -1,
        // Disable Disable Timeout resource
        .disable_timeout = -1,
        // Sets preferred transport
        .binding = MAIN_PREFERRED_TRANSPORT
    };

    // Anjay will assign Instance ID automatically
    anjay_iid_t server_instance_id = ANJAY_ID_INVALID;
    if (anjay_server_object_add_instance(anjay, &server_instance,
                                         &server_instance_id)) {
        return -1;
    }

    return 0;
}


void anjay_init(void) {
    const anjay_configuration_t CONFIG = {
        .endpoint_name = ENDPOINT_NAME,
        .in_buffer_size = 4000,
        .out_buffer_size = 4000,
        .msg_cache_size = 4000
    };

    // Read necessary data for object install
    read_anjay_config();

    anjay = anjay_new(&CONFIG);
    if (!anjay) {
        avs_log(tutorial, ERROR, "Could not create Anjay object");
        return;
    }

    if (setup_security_object(anjay) || setup_server_object(anjay)
            || fw_update_install(anjay)) {
        avs_log(tutorial, ERROR, "Failed to install core objects");
        return;
    }

   if (!(DEVICE_OBJ = device_object_create())
            || anjay_register_object(anjay, DEVICE_OBJ)) {
        avs_log(tutorial, ERROR, "Could not register Device object");
        return;
    }

    sensors_install(anjay);
}

void anjay_task(void *pvParameters) {

    while (true) {
        otDeviceRole role = otThreadGetDeviceRole(esp_openthread_get_instance());

        if (role == OT_DEVICE_ROLE_CHILD || 
            role == OT_DEVICE_ROLE_ROUTER || 
            role == OT_DEVICE_ROLE_LEADER) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }


    update_connection_status_job(anjay_get_scheduler(anjay), &anjay);
    update_objects_job(anjay_get_scheduler(anjay), &anjay);

    anjay_event_loop_run(anjay, avs_time_duration_from_scalar(1, AVS_TIME_S));

    avs_sched_del(&sensors_job_handle);
    avs_sched_del(&connection_status_job_handle);
    anjay_delete(anjay);
    sensors_release();
}
