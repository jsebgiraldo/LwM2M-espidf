/*
 * Copyright 2022 AVSystem <avsystem@avsystem.com>
 * AVSystem Anjay LwM2M SDK
 * ALL RIGHTS RESERVED
 */
#ifndef _CELLULAR_SETUP_H
#define _CELLULAR_SETUP_H

/* FreeRTOS Cellular Library include. */
#include "cellular_config.h"
#include "cellular_config_defaults.h"
#include "cellular_types.h"

/* Secure socket needs application to provide the cellular handle and pdn context id. */
/* User of secure sockets cellular should provide this variable. */
CellularHandle_t CellularHandle;

/* User of secure sockets cellular should provide this variable. */
uint8_t CellularSocketPdnContextId;

bool setupCellular( void );

#endif
