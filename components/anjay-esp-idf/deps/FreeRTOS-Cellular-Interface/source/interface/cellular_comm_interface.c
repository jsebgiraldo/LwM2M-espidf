/*
 * Copyright 2022 AVSystem <avsystem@avsystem.com>
 * AVSystem Anjay LwM2M SDK
 * ALL RIGHTS RESERVED
 */

#include "freertos/FreeRTOS.h"
#ifndef CELLULAR_DO_NOT_USE_CUSTOM_CONFIG
    /* Include custom config file before other headers. */
    #include "cellular_config.h"
#endif
#include "cellular_config_defaults.h"
#include "cellular_comm_interface.h"
#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include <avsystem/commons/avs_time.h>
#include <avsystem/commons/avs_log.h>

#define BUFFER_SIZE 512U
#define QUEUE_SIZE  64U

#define UNUSED_PIN -1

#define UART_TASK_HEAP_SIZE 2048U
#define UART_TASK_PRIORITY 12U

/*-----------------------------------------------------------*/

/**
 * @brief A context of the communication interface.
 */
typedef struct cellular_comm_interface_context
{
    CellularCommInterfaceReceiveCallback_t recv_callback;   /**< CellularCommInterfaceReceiveCallback_t received callback function set in open function. */
    void *user_data;                                        /**< user_data in CellularCommInterfaceReceiveCallback_t callback function. */
    QueueHandle_t uart_queue;
    TaskHandle_t uart_task_handle;
} cellular_comm_interface_context_t;

/*-----------------------------------------------------------*/

static CellularCommInterfaceError_t cellular_open(CellularCommInterfaceReceiveCallback_t receive_callback, void *user_data, CellularCommInterfaceHandle_t *comm_interface_handle);

static CellularCommInterfaceError_t cellular_close(CellularCommInterfaceHandle_t comm_interface_handle);

static CellularCommInterfaceError_t cellular_receive(CellularCommInterfaceHandle_t comm_interface_handle,
                                                       uint8_t *buffer,
                                                       uint32_t buffer_length,
                                                       uint32_t timeout_milliseconds,
                                                       uint32_t *data_received_length);

static CellularCommInterfaceError_t cellular_send(CellularCommInterfaceHandle_t comm_interface_handle,
                                                    const uint8_t *data,
                                                    uint32_t data_length,
                                                    uint32_t timeout_milliseconds,
                                                    uint32_t *data_sent_length);

/*-----------------------------------------------------------*/

CellularCommInterface_t CellularCommInterface =
{
    .open = cellular_open,
    .send = cellular_send,
    .recv = cellular_receive,
    .close = cellular_close
};

/*-----------------------------------------------------------*/

static cellular_comm_interface_context_t comm_intf_ctx = {0};

/*-----------------------------------------------------------*/

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    for (;;)
    {
        if (xQueueReceive(comm_intf_ctx.uart_queue, (void *)&event, (portTickType)portMAX_DELAY))
        {
            switch (event.type)
            {
                case UART_DATA:
                    avs_log(tutorial, TRACE, "data received");
                    comm_intf_ctx.recv_callback(comm_intf_ctx.user_data, (CellularCommInterfaceHandle_t)&comm_intf_ctx);
                    break;
                case UART_FIFO_OVF:
                    avs_log(tutorial, TRACE, "hw fifo overflow");
                    uart_flush_input(CONFIG_ANJAY_BG96_UART_PORT_NUMBER);
                    xQueueReset(comm_intf_ctx.uart_queue);
                    break;
                case UART_BUFFER_FULL:
                    avs_log(tutorial, TRACE, "ring buffer full");
                    uart_flush_input(CONFIG_ANJAY_BG96_UART_PORT_NUMBER);
                    xQueueReset(comm_intf_ctx.uart_queue);
                    break;
                case UART_BREAK:
                    avs_log(tutorial, TRACE, "uart rx break");
                    break;
                case UART_PARITY_ERR:
                    avs_log(tutorial, TRACE, "uart parity error");
                    break;
                case UART_FRAME_ERR:
                    avs_log(tutorial, TRACE, "uart frame error");
                    break;
                default:
                    avs_log(tutorial, TRACE, "uart event type: %d", event.type);
                    break;
            }
        }
    }
}

/*-----------------------------------------------------------*/

static CellularCommInterfaceError_t cellular_open(CellularCommInterfaceReceiveCallback_t receive_callback,
                                                    void *user_data,
                                                    CellularCommInterfaceHandle_t *comm_interface_handle)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    if (uart_param_config(CONFIG_ANJAY_BG96_UART_PORT_NUMBER, &uart_config)) {
        return IOT_COMM_INTERFACE_DRIVER_ERROR;
    }

    if (uart_set_pin(CONFIG_ANJAY_BG96_UART_PORT_NUMBER, CONFIG_ANJAY_BG96_TX_PIN, CONFIG_ANJAY_BG96_RX_PIN, UNUSED_PIN, UNUSED_PIN)) {
        return IOT_COMM_INTERFACE_DRIVER_ERROR;
    }

    if (uart_driver_install(CONFIG_ANJAY_BG96_UART_PORT_NUMBER, BUFFER_SIZE,
                                        BUFFER_SIZE, QUEUE_SIZE, &comm_intf_ctx.uart_queue, 0)) {
        return IOT_COMM_INTERFACE_DRIVER_ERROR;
    }

    if (!comm_intf_ctx.uart_queue) {
        return IOT_COMM_INTERFACE_NO_MEMORY;
    }

    comm_intf_ctx.recv_callback = receive_callback;
    comm_intf_ctx.user_data = user_data;
    xTaskCreate(uart_event_task, "uart_event_task", UART_TASK_HEAP_SIZE, NULL, UART_TASK_PRIORITY, &comm_intf_ctx.uart_task_handle);

    *comm_interface_handle = (CellularCommInterfaceHandle_t)&comm_intf_ctx;
    return IOT_COMM_INTERFACE_SUCCESS;
}

static CellularCommInterfaceError_t cellular_close(CellularCommInterfaceHandle_t comm_interface_handle)
{
    cellular_comm_interface_context_t *ctx = (cellular_comm_interface_context_t*)comm_interface_handle;

    if (uart_driver_delete(CONFIG_ANJAY_BG96_UART_PORT_NUMBER)) {
        return IOT_COMM_INTERFACE_DRIVER_ERROR;
    }

    vTaskDelete(ctx->uart_task_handle);

    return IOT_COMM_INTERFACE_SUCCESS;
}

static CellularCommInterfaceError_t cellular_receive(CellularCommInterfaceHandle_t comm_interface_handle,
                                                       uint8_t *buffer,
                                                       uint32_t buffer_length,
                                                       uint32_t timeout_milliseconds,
                                                       uint32_t *data_received_length)
{
    (void) comm_interface_handle;

    uint32_t data_length = 0;
    int ret = 0;

    if (uart_get_buffered_data_len(CONFIG_ANJAY_BG96_UART_PORT_NUMBER, (uint32_t*)&data_length)) {
        return IOT_COMM_INTERFACE_DRIVER_ERROR;
    }

    if (data_length > buffer_length) {
        return IOT_COMM_INTERFACE_FAILURE;
    }

    ret = uart_read_bytes(CONFIG_ANJAY_BG96_UART_PORT_NUMBER, buffer, data_length, pdMS_TO_TICKS(timeout_milliseconds));
    if (ret < 0) {
        return IOT_COMM_INTERFACE_DRIVER_ERROR;
    }

    *data_received_length = (uint32_t)ret;

    return IOT_COMM_INTERFACE_SUCCESS;
}

static CellularCommInterfaceError_t cellular_send(CellularCommInterfaceHandle_t comm_interface_handle,
                                                    const uint8_t *data,
                                                    uint32_t data_length,
                                                    uint32_t timeout_milliseconds,
                                                    uint32_t *data_sent_length)
{
    (void) comm_interface_handle;

    int ret = 0;
    uint32_t data_to_send = data_length;
    int64_t start_timestamp = 0;
    int64_t current_timestamp = 0;

    if (avs_time_real_to_scalar(&start_timestamp, AVS_TIME_MS,
                                avs_time_real_now())) {
        return IOT_COMM_INTERFACE_DRIVER_ERROR;
    }

    while (data_to_send > 0) {
        ret = uart_tx_chars(CONFIG_ANJAY_BG96_UART_PORT_NUMBER, (const char *)&data[data_length - data_to_send], data_to_send);
        if (ret < 0) {
            return IOT_COMM_INTERFACE_DRIVER_ERROR;
        }

        data_to_send -= ret;
        *data_sent_length = data_length - data_to_send;

        if (avs_time_real_to_scalar(&current_timestamp, AVS_TIME_MS,
                            avs_time_real_now())) {
            return IOT_COMM_INTERFACE_DRIVER_ERROR;
        }

        if ((current_timestamp - start_timestamp) >= timeout_milliseconds) {
            return IOT_COMM_INTERFACE_TIMEOUT;
        }
    }

    return IOT_COMM_INTERFACE_SUCCESS;
}
