#include "advanced_firmware_update.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define AFU_VERSION_STR_MAX_LEN 10
#define AFU_INSTANCE_NAME_STR_MAX_LEN 10
#define AFU_FILE_NAME_STR_MAX_LEN 50

typedef struct {
    anjay_t *anjay;
    char fw_version[AFU_NUMBER_OF_FIRMWARE_INSTANCES]
                   [AFU_VERSION_STR_MAX_LEN + 1];
    char instance_name[AFU_NUMBER_OF_FIRMWARE_INSTANCES]
                      [AFU_INSTANCE_NAME_STR_MAX_LEN + 1];
    FILE *new_firmware_file[AFU_NUMBER_OF_FIRMWARE_INSTANCES];
} advanced_firmware_update_logic_t;

static advanced_firmware_update_logic_t afu_logic;

const char *ENDPOINT_NAME;

static void get_firmware_download_name(int iid, char *buff) {
    if (iid == AFU_DEFAULT_FIRMWARE_INSTANCE_IID) {
        snprintf(buff, AFU_FILE_NAME_STR_MAX_LEN, "/tmp/firmware_image.bin");
    } else {
        snprintf(buff, AFU_FILE_NAME_STR_MAX_LEN, "/tmp/add_image_%d", iid);
    }
}

static void get_add_firmware_file_name(int iid, char *buff) {
    snprintf(buff, AFU_FILE_NAME_STR_MAX_LEN, "ADD_FILE_%d", iid);
}

static void get_marker_file_name(int iid, char *buff) {
    snprintf(buff, AFU_FILE_NAME_STR_MAX_LEN, "/tmp/fw-updated-marker_%d", iid);
}

static int move_file(const char *dest, const char *source) {
    int ret_val = -1;
    FILE *dest_stream = NULL;
    FILE *source_stream = fopen(source, "r");

    if (!source_stream) {
        avs_log(advance_fu, ERROR, "Could not open file: %s", source);
        goto cleanup;
    }
    dest_stream = fopen(dest, "w");
    if (!dest_stream) {
        avs_log(advance_fu, ERROR, "Could not open file: %s", dest);
        fclose(source_stream);
        goto cleanup;
    }

    while (!feof(source_stream)) {
        char buff[1024];
        size_t bytes_read_1 = fread(buff, 1, sizeof(buff), source_stream);
        if (fwrite(buff, 1, bytes_read_1, dest_stream) != bytes_read_1) {
            avs_log(advance_fu, ERROR, "Error during write to file: %s", dest);
            goto cleanup;
        }
    }
    ret_val = 0;

cleanup:
    if (dest_stream) {
        if (fclose(dest_stream)) {
            avs_log(advance_fu, ERROR, "Could not close file: %s", dest);
            ret_val = -1;
        }
    }
    if (source_stream) {
        if (fclose(source_stream)) {
            avs_log(advance_fu, ERROR, "Could not close file: %s", source);
            ret_val = -1;
        }
    }
    unlink(source);

    return ret_val;
}

static void add_conflicting_instance(anjay_iid_t iid, anjay_iid_t target_iid) {
    const anjay_iid_t *conflicting_instances;
    anjay_iid_t conflicting_target_iids[AFU_NUMBER_OF_FIRMWARE_INSTANCES - 1];
    size_t conflicting_iids_count = 0;

    // get conflicting instances
    anjay_advanced_fw_update_get_conflicting_instances(afu_logic.anjay, iid,
                                                       &conflicting_instances,
                                                       &conflicting_iids_count);
    // add target_iid to the list
    for (size_t i = 0; i < conflicting_iids_count; i++) {
        conflicting_target_iids[i] = conflicting_instances[i];
    }
    conflicting_target_iids[conflicting_iids_count++] = target_iid;
    anjay_advanced_fw_update_set_conflicting_instances(afu_logic.anjay, iid,
                                                       conflicting_target_iids,
                                                       conflicting_iids_count);
}

static bool is_update_requested(anjay_iid_t iid,
                                anjay_iid_t target_iid,
                                const anjay_iid_t *requested_supplemental_iids,
                                size_t requested_supplemental_iids_count,
                                const anjay_iid_t *linked_target_iids,
                                size_t linked_iids_count) {
    if (iid == target_iid) {
        return true;
    }
    if (requested_supplemental_iids) {
        for (size_t i = 0; i < requested_supplemental_iids_count; i++) {
            if (iid == requested_supplemental_iids[i]) {
                return true;
            }
        }
    } else if (linked_target_iids) {
        for (size_t i = 0; i < linked_iids_count; i++) {
            if (iid == linked_target_iids[i]) {
                return true;
            }
        }
    }
    return false;
}

static char get_firmware_major_version(anjay_iid_t iid, bool is_upgrade) {
    char file_name[AFU_FILE_NAME_STR_MAX_LEN] = { 0 };

    if (is_upgrade == false) {
        return afu_logic.fw_version[iid][0];
    }

    get_firmware_download_name(iid, file_name);

    // get value from new file
    char buff;
    FILE *stream = fopen(file_name, "r");
    if (!stream) {
        avs_log(advance_fu, ERROR, "Could not open file: %s", file_name);
        return ' ';
    }
    if (!fread(&buff, 1, 1, stream)) {
        avs_log(advance_fu, ERROR, "Could not read from file file: %s",
                file_name);
        fclose(stream);
        return ' ';
    }
    if (fclose(stream)) {
        avs_log(advance_fu, ERROR, "Could not close file: %s", file_name);
    }

    return buff;
}

static int refresh_fw_version() {
    memcpy(afu_logic.fw_version[AFU_DEFAULT_FIRMWARE_INSTANCE_IID],
           AFU_DEFAULT_FIRMWARE_VERSION, strlen(AFU_DEFAULT_FIRMWARE_VERSION));

    for (int i = 1; i < AFU_NUMBER_OF_FIRMWARE_INSTANCES; i++) {
        char buff[AFU_FILE_NAME_STR_MAX_LEN] = { 0 };
        get_add_firmware_file_name(i, buff);
        FILE *stream = fopen(buff, "r");
        if (!stream) {
            avs_log(advance_fu, ERROR, "Could not open file with iid: %d", i);
            return -1;
        }
        if (!fread(afu_logic.fw_version[i], 1, AFU_VERSION_STR_MAX_LEN,
                   stream)) {
            avs_log(advance_fu, ERROR, "Could not read file with iid: %d", i);
            fclose(stream);
            return -1;
        }
        if (fclose(stream)) {
            avs_log(advance_fu, ERROR, "Could not close file with iid: %d", i);
            return -1;
        }
    }

    for (int i = 0; i < AFU_NUMBER_OF_FIRMWARE_INSTANCES; i++) {
        avs_log(advance_fu, INFO,
                "Firmware version for object with IID %d is: %s", i,
                afu_logic.fw_version[i]);
    }

    return 0;
}

static int fw_stream_open(anjay_iid_t iid, void *user_ptr) {
    (void) user_ptr;

    char file_name[AFU_FILE_NAME_STR_MAX_LEN] = { 0 };
    get_firmware_download_name(iid, file_name);

    if (afu_logic.new_firmware_file[iid]) {
        avs_log(advance_fu, ERROR, "Already open %s", file_name);
        return -1;
    }
    afu_logic.new_firmware_file[iid] = fopen(file_name, "wb");
    if (!afu_logic.new_firmware_file[iid]) {
        avs_log(advance_fu, ERROR, "Could not open %s", file_name);
        return -1;
    }
    return 0;
}

static int fw_update_common_write(anjay_iid_t iid,
                                  void *user_ptr,
                                  const void *data,
                                  size_t length) {
    (void) user_ptr;

    if (!afu_logic.new_firmware_file[iid]) {
        avs_log(advance_fu, ERROR, "Stream not open: object %d", iid);
        return -1;
    }
    if (length
            && (fwrite(data, length, 1, afu_logic.new_firmware_file[iid]) != 1
                || fflush(afu_logic.new_firmware_file[iid]) != 0)) {
        avs_log(advance_fu, ERROR, "fwrite or fflush failed: %s",
                strerror(errno));
        return ANJAY_ADVANCED_FW_UPDATE_ERR_NOT_ENOUGH_SPACE;
    }
    return 0;
}

static void remove_linked_instance(anjay_iid_t iid, anjay_iid_t target_iid) {
    const anjay_iid_t *linked_instances;
    anjay_iid_t linked_target_iids[AFU_NUMBER_OF_FIRMWARE_INSTANCES - 1];
    size_t linked_iids_count = 0;
    size_t new_linked_iids_count = 0;

    // get linked instances
    anjay_advanced_fw_update_get_linked_instances(
            afu_logic.anjay, iid, &linked_instances, &linked_iids_count);
    // remove target_iid from the list
    for (size_t i = 0; i < linked_iids_count; i++) {
        if (linked_instances[i] != target_iid) {
            linked_target_iids[new_linked_iids_count++] = linked_instances[i];
        }
    }
    // update linked list
    anjay_advanced_fw_update_set_linked_instances(
            afu_logic.anjay, iid, linked_target_iids, new_linked_iids_count);
}

static void remove_conflicting_instance(anjay_iid_t iid,
                                        anjay_iid_t target_iid) {
    const anjay_iid_t *conflicting_instances;
    anjay_iid_t conflicting_target_iids[AFU_NUMBER_OF_FIRMWARE_INSTANCES - 1];
    size_t conflicting_iids_count = 0;
    size_t new_conflicting_iids_count = 0;

    // get conflicting instances
    anjay_advanced_fw_update_get_conflicting_instances(afu_logic.anjay, iid,
                                                       &conflicting_instances,
                                                       &conflicting_iids_count);
    // remove target_iid from the list
    for (size_t i = 0; i < conflicting_iids_count; i++) {
        if (conflicting_instances[i] != target_iid) {
            conflicting_target_iids[new_conflicting_iids_count++] =
                    conflicting_instances[i];
        }
    }
    // update conflicting list
    anjay_advanced_fw_update_set_conflicting_instances(
            afu_logic.anjay, iid, conflicting_target_iids,
            new_conflicting_iids_count);
}

static void add_linked_instance(anjay_iid_t iid, anjay_iid_t target_iid) {
    const anjay_iid_t *linked_instances;
    anjay_iid_t linked_target_iids[AFU_NUMBER_OF_FIRMWARE_INSTANCES - 1];
    size_t linked_iids_count = 0;

    // get linked instances
    anjay_advanced_fw_update_get_linked_instances(
            afu_logic.anjay, iid, &linked_instances, &linked_iids_count);
    // add target_iid to the list
    for (size_t i = 0; i < linked_iids_count; i++) {
        linked_target_iids[i] = linked_instances[i];
    }
    linked_target_iids[linked_iids_count++] = target_iid;
    anjay_advanced_fw_update_set_linked_instances(
            afu_logic.anjay, iid, linked_target_iids, linked_iids_count);
}

static int fw_update_common_finish(anjay_iid_t iid, void *user_ptr) {
    (void) user_ptr;

    if (!afu_logic.new_firmware_file[iid]) {
        avs_log(advance_fu, ERROR, "Stream not open: object %d", iid);
        return -1;
    }

    if (fclose(afu_logic.new_firmware_file[iid])) {
        avs_log(advance_fu, ERROR, "Closing firmware image failed: object %d",
                iid);
        afu_logic.new_firmware_file[iid] = NULL;
        return -1;
    }
    afu_logic.new_firmware_file[iid] = NULL;

    /*
    If other firmware instances are in downloaded state set linked instances,
    based on them, the upgrade will be performed simultaneously in the
    perform_upgrade callback. The reason for setting linked instances may be
    different and depends on the user's implementation, but always mean
    that instances will be updated in a batch if the Update resource is executed
    with no arguments.
    */
    for (anjay_iid_t i = 0; i < AFU_NUMBER_OF_FIRMWARE_INSTANCES; i++) {
        if (i != iid) {
            anjay_advanced_fw_update_state_t state;
            anjay_advanced_fw_update_get_state(afu_logic.anjay, i, &state);
            if (state == ANJAY_ADVANCED_FW_UPDATE_STATE_DOWNLOADED) {
                add_linked_instance(iid, i);
                add_linked_instance(i, iid);
            }
        }
    }

    return 0;
}

static int
fw_update_common_perform_upgrade(anjay_iid_t iid,
                                 void *user_ptr,
                                 const anjay_iid_t *requested_supplemental_iids,
                                 size_t requested_supplemental_iids_count) {
    (void) user_ptr;

    const anjay_iid_t *linked_target_iids;
    bool update_iid[AFU_NUMBER_OF_FIRMWARE_INSTANCES];
    size_t linked_iids_count = 0;

    // get linked instances
    anjay_advanced_fw_update_get_linked_instances(
            afu_logic.anjay, iid, &linked_target_iids, &linked_iids_count);

    /* Prepare list of iid to update. If requested_supplemental_iids is present
     * use it otherwise use linked_target_iids.*/
    for (anjay_iid_t i = 0; i < AFU_NUMBER_OF_FIRMWARE_INSTANCES; i++) {
        if (is_update_requested(i, iid, requested_supplemental_iids,
                                requested_supplemental_iids_count,
                                linked_target_iids, linked_iids_count)) {
            update_iid[i] = true;
            // check if new file is already downloaded
            anjay_advanced_fw_update_state_t state;
            anjay_advanced_fw_update_get_state(afu_logic.anjay, i, &state);
            if ((i != iid)
                    && (state != ANJAY_ADVANCED_FW_UPDATE_STATE_DOWNLOADED)) {
                avs_log(advance_fu, ERROR,
                        "Upgrade can't be performed, firmware file with iid %d "
                        "is not ready",
                        i);
                // set conflicting instance
                add_conflicting_instance(iid, i);
                return ANJAY_ADVANCED_FW_UPDATE_ERR_CONFLICTING_STATE;
            }
        } else {
            update_iid[i] = false;
        }
    }

    /*
    Check firmware version compatibility.
    In this example major version number is compare - first character in every
    additional image must have the same value. If new file is given (DOWNLOADED
    STATE), get this value from them, otherwise use the old one.
    */
    for (anjay_iid_t i = 1; i < AFU_NUMBER_OF_FIRMWARE_INSTANCES; i++) {
        if (update_iid[i] == true) {
            for (anjay_iid_t j = i + 1; j < AFU_NUMBER_OF_FIRMWARE_INSTANCES;
                 j++) {
                if (get_firmware_major_version(i, update_iid[i])
                        != get_firmware_major_version(j, update_iid[j])) {
                    avs_log(advance_fu, ERROR,
                            "Upgrade can't be performed, conflicting firmware "
                            "version between instance %d and %d",
                            i, j);
                    // set conflicting instance due to firmware version
                    // incompatibility
                    add_conflicting_instance(i, j);
                    add_conflicting_instance(j, i);
                    return ANJAY_ADVANCED_FW_UPDATE_ERR_CONFLICTING_STATE;
                }
            }
        }
    }

    /* No errors found, change the status of all requested_supplemental_iids or
     * linked_target_iids to UPDATING before the actual update process.*/
    for (anjay_iid_t i = 0; i < AFU_NUMBER_OF_FIRMWARE_INSTANCES; i++) {
        if (update_iid[i] == true) {
            if (i != iid) {
                anjay_advanced_fw_update_set_state_and_result(
                        afu_logic.anjay, i,
                        ANJAY_ADVANCED_FW_UPDATE_STATE_UPDATING,
                        ANJAY_ADVANCED_FW_UPDATE_RESULT_INITIAL);
            }
        }
    }

    // after firmware versions check, start firmware update, first with
    // additional images
    for (anjay_iid_t i = 1; i < AFU_NUMBER_OF_FIRMWARE_INSTANCES; i++) {
        if (update_iid[i] == true) {
            avs_log(advance_fu, INFO, "Perform update on %d instance", i);

            char new_firm_name[AFU_FILE_NAME_STR_MAX_LEN] = { 0 };
            char current_firm_name[AFU_FILE_NAME_STR_MAX_LEN] = { 0 };
            get_firmware_download_name(i, new_firm_name);
            get_add_firmware_file_name(i, current_firm_name);

            if (move_file(current_firm_name, new_firm_name)) {
                avs_log(advance_fu, ERROR,
                        "Error during the %d additional image swapping", i);
                return -1;
            }
            // if main application is restarted, set update marker
            if (update_iid[AFU_DEFAULT_FIRMWARE_INSTANCE_IID] == true) {
                char marker_name[AFU_FILE_NAME_STR_MAX_LEN] = { 0 };
                get_marker_file_name(i, marker_name);
                FILE *marker = fopen(marker_name, "w");
                if (!marker) {
                    avs_log(advance_fu, ERROR,
                            "Marker file could not be created");
                    return -1;
                }
                if (fclose(marker)) {
                    avs_log(advance_fu, ERROR,
                            "Marker file could not be close");
                }
            } // if main application is not restarted, update state
            else {
                anjay_advanced_fw_update_set_state_and_result(
                        afu_logic.anjay, i, ANJAY_ADVANCED_FW_UPDATE_STATE_IDLE,
                        ANJAY_ADVANCED_FW_UPDATE_RESULT_SUCCESS);
            }
        }
    }

    // update application
    if (update_iid[AFU_DEFAULT_FIRMWARE_INSTANCE_IID] == true) {
        avs_log(advance_fu, INFO, "Perform update on default instance");

        char new_firm_name[AFU_FILE_NAME_STR_MAX_LEN] = { 0 };
        char marker_name[AFU_FILE_NAME_STR_MAX_LEN] = { 0 };
        get_firmware_download_name(AFU_DEFAULT_FIRMWARE_INSTANCE_IID,
                                   new_firm_name);
        get_marker_file_name(AFU_DEFAULT_FIRMWARE_INSTANCE_IID, marker_name);

        if (chmod(new_firm_name, 0700) == -1) {
            avs_log(advance_fu, ERROR, "Could not make firmware executable: %s",
                    strerror(errno));
            return -1;
        }
        // Create a marker file, so that the new process knows it is the
        // "upgraded" one
        FILE *marker = fopen(marker_name, "w");
        if (!marker) {
            avs_log(advance_fu, ERROR, "Marker file could not be created");
            return -1;
        }
        if (fclose(marker)) {
            avs_log(advance_fu, ERROR, "Marker file could not be close");
        }

        assert(ENDPOINT_NAME);

        // If the call below succeeds, the firmware is considered as "upgraded",
        // and we hope the newly started client registers to the Server.
        (void) execl(new_firm_name, new_firm_name, ENDPOINT_NAME, NULL);
        avs_log(advance_fu, ERROR, "execl() failed: %s", strerror(errno));
        // If we are here, it means execl() failed. Marker file MUST now be
        // removed, as the firmware update failed.
        unlink(marker_name);
        return -1;
    }

    // update firmware version
    refresh_fw_version();

    // clear conflicting and linked instances in the objects
    for (anjay_iid_t i = 0; i < AFU_NUMBER_OF_FIRMWARE_INSTANCES; i++) {
        if (update_iid[i] == true) {
            anjay_advanced_fw_update_set_conflicting_instances(afu_logic.anjay,
                                                               i, NULL, 0);
            anjay_advanced_fw_update_set_linked_instances(afu_logic.anjay, i,
                                                          NULL, 0);
            // clear conflicting and linked instances about this object from
            // other objects
            for (anjay_iid_t j = 0; j < AFU_NUMBER_OF_FIRMWARE_INSTANCES; j++) {
                if (i != j) {
                    remove_linked_instance(i, j);
                    remove_conflicting_instance(i, j);
                }
            }
        }
    }

    return 0;
}

void fw_update_common_reset(anjay_iid_t iid, void *user_ptr) {
    (void) user_ptr;

    char new_firm_name[AFU_FILE_NAME_STR_MAX_LEN] = { 0 };
    get_firmware_download_name(iid, new_firm_name);

    // Reset can be issued even if the download never started
    if (afu_logic.new_firmware_file[iid]) {
        // We ignore the result code of fclose(), as fw_reset() can't fail
        (void) fclose(afu_logic.new_firmware_file[iid]);
        // and reset our global state to initial value
        afu_logic.new_firmware_file[iid] = NULL;
    }
    // Finally, let's remove any downloaded payload
    unlink(new_firm_name);

    // clear conflicting and linked instances in the object
    anjay_advanced_fw_update_set_conflicting_instances(afu_logic.anjay, iid,
                                                       NULL, 0);
    anjay_advanced_fw_update_set_linked_instances(afu_logic.anjay, iid, NULL,
                                                  0);
    // clear conflicting and linked instances about this object from other
    // objects
    for (anjay_iid_t i = 0; i < AFU_NUMBER_OF_FIRMWARE_INSTANCES; i++) {
        if (i != iid) {
            remove_linked_instance(i, iid);
            remove_conflicting_instance(i, iid);
        }
    }
}

static const char *fw_update_common_get_current_version(anjay_iid_t iid,
                                                        void *user_ptr) {
    (void) user_ptr;

    return (const char *) afu_logic.fw_version[iid];
}

static const anjay_advanced_fw_update_handlers_t handlers = {
    .stream_open = fw_stream_open,
    .stream_write = fw_update_common_write,
    .stream_finish = fw_update_common_finish,
    .reset = fw_update_common_reset,
    .get_current_version = fw_update_common_get_current_version,
    .perform_upgrade = fw_update_common_perform_upgrade
};

int afu_update_install(anjay_t *anjay) {
    anjay_advanced_fw_update_initial_state_t state;
    char marker_name[AFU_FILE_NAME_STR_MAX_LEN] = { 0 };
    char file_name[AFU_FILE_NAME_STR_MAX_LEN] = { 0 };

    memset(&state, 0, sizeof(state));

    afu_logic.anjay = anjay;

    anjay_advanced_fw_update_global_config_t config = {
        .prefer_same_socket_downloads = true
    };
    int result = anjay_advanced_fw_update_install(anjay, &config);
    if (result) {
        avs_log(advance_fu, ERROR,
                "Could not install advanced firmware object: %d", result);
        return -1;
    }

    // check if application was updated
    get_marker_file_name(AFU_DEFAULT_FIRMWARE_INSTANCE_IID, marker_name);
    if (access(marker_name, F_OK) != -1) {
        avs_log(advance_fu, INFO, "Application update succeded");
        state.result = ANJAY_ADVANCED_FW_UPDATE_RESULT_SUCCESS;
        unlink(marker_name);
    }
    result = anjay_advanced_fw_update_instance_add(
            anjay, AFU_DEFAULT_FIRMWARE_INSTANCE_IID, "application", &handlers,
            NULL, &state);
    if (result) {
        avs_log(advance_fu, ERROR,
                "Could not add default application instance: %d", result);
        return -1;
    }

    // check if additional files were updated, if not create it with default
    // value
    for (anjay_iid_t i = 1; i < AFU_NUMBER_OF_FIRMWARE_INSTANCES; i++) {
        memset(marker_name, 0, sizeof(marker_name));
        get_marker_file_name(i, marker_name);
        if (access(marker_name, F_OK) != -1) {
            avs_log(advance_fu, INFO,
                    "Additional file with idd: %d update succeded", i);
            state.result = ANJAY_ADVANCED_FW_UPDATE_RESULT_SUCCESS;
            unlink(marker_name);
        } else {
            state.result = ANJAY_ADVANCED_FW_UPDATE_RESULT_INITIAL;
        }

        memset(file_name, 0, sizeof(file_name));
        get_add_firmware_file_name(i, file_name);
        // create file only if not exist
        if (access(file_name, F_OK) != 0) {
            FILE *stream = fopen(file_name, "wb");
            if (!stream) {
                avs_log(advance_fu, ERROR, "Could not open %s", file_name);
                return -1;
            }
            if (fwrite(AFU_ADD_FILE_DEFAULT_CONTENT,
                       strlen(AFU_ADD_FILE_DEFAULT_CONTENT), 1, stream)
                    != 1) {
                avs_log(advance_fu, ERROR, "Could not write to %s", file_name);
                fclose(stream);
                return -1;
            }
            if (fclose(stream)) {
                avs_log(advance_fu, ERROR, "Could not close %s", file_name);
                return -1;
            }
        }

        snprintf(afu_logic.instance_name[i], AFU_INSTANCE_NAME_STR_MAX_LEN,
                 "add_img_%d", i);
        result = anjay_advanced_fw_update_instance_add(
                anjay, i, afu_logic.instance_name[i], &handlers, NULL, &state);
        if (result) {
            avs_log(advance_fu, ERROR,
                    "Could not add the additional image instance: %d", result);
            return -1;
        }
    }

    if (refresh_fw_version()) {
        return -1;
    }

    return 0;
}
