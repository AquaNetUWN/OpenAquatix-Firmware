/*
 * mac_main.h
 *
 *  Created on: Sep 8, 2025
 *      Author: ericv
 * 
 * Copyright (c) 2025 OpenAquatix Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef MAC_MAC_MAIN_H_
#define MAC_MAC_MAIN_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "mess_main.h"
#include <stdbool.h>
#include <stdint.h>


/* Private includes ----------------------------------------------------------*/



/* Exported types ------------------------------------------------------------*/

typedef enum {
  MAC_STATE_SUCCESS,
  MAC_STATE_DEFERRED,
  MAC_STATE_DROPPED,
  MAC_STATE_ERROR,
  MAC_STATE_IDLE
} MacState_t;

typedef struct {
  bool host_mode_enabled;
  uint32_t regular_tx_depth;
  uint32_t emergency_tx_depth;
  bool scheduled_tx_pending;
  uint32_t scheduled_tx_id;
  uint32_t scheduled_tx_cyccnt;
} MacHostStatus_t;

/* Exported constants --------------------------------------------------------*/



/* Exported macro ------------------------------------------------------------*/



/* Exported functions prototypes ---------------------------------------------*/

void MAC_StartTask(void* argument);
bool MAC_IsHostModeEnabled(void);
bool MAC_ScheduleHostMessage(const Message_t* message, uint32_t* request_id);
bool MAC_CancelHostMessage(uint32_t request_id);
bool MAC_GetHostStatus(MacHostStatus_t* status);

/* Private defines -----------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif /* MAC_MAC_MAIN_H_ */
