/*
 * mac_main.c
 *
 *  Created on: Sep 8, 2025
 *      Author: ericv
 * 
 * Copyright (c) 2025 OpenAquatix Contributors
 * SPDX-License-Identifier: MIT
 */

/* Private includes ----------------------------------------------------------*/

#include "cfg_main.h"
#include "cfg_parameters.h"
#include "cfg_defaults.h"
#include "comm_main.h"
#include "mess_main.h"
#include "mac_csma_ca_beb.h"
#include "mac_no_mac.h"
#include "mac_protocol.h"
#include "error_manager.h"
#include "cmsis_os.h"
#include <stdbool.h>
#include <limits.h>
#include <string.h>

/* Private typedef -----------------------------------------------------------*/

typedef struct {
  MacState_t state;

  MacProtocol_t current_protocol;
  MacProtocol_t requested_protocol;

  const MacProtocolInterface_t* interface;

  union {
    NoMacData_t no_mac_data;
    CsmaCaBebData_t csma_ca_beb_data;
  } protocol_data;
} MacTaskContext_t;

typedef struct {
  bool pending;
  uint32_t request_id;
  Message_t message;
} HostScheduledMessage_t;

/* Private define ------------------------------------------------------------*/

#define REGULAR_TX_QUEUE_SIZE       5
#define EMERGENCY_TX_QUEUE_SIZE     3
#define RX_QUEUE_SIZE               1
#define HOST_TX_MIN_LEAD_MS         300U
#define HOST_TX_HANDOFF_LEAD_MS     400U

/* Private macro -------------------------------------------------------------*/



/* Private variables ---------------------------------------------------------*/

DEFINE_DESC_TABLE(MAC_PROTOCOL_TABLE, mac_protocol_descriptions)

osMessageQueueId_t regular_tx_queue = NULL;
osMessageQueueId_t emergency_tx_queue = NULL;
osMessageQueueId_t mac_rx_queue = NULL;

static MacTaskContext_t task_context = {
  .state = MAC_STATE_IDLE,
  .current_protocol = MAC_PROTOCOL_UNKNOWN,
  .requested_protocol = DEFAULT_MAC,
  .interface = NULL
};
static HostScheduledMessage_t host_scheduled_message;
static uint32_t next_host_request_id = 1;
static osMutexId_t host_state_mutex = NULL;

extern const MacProtocolInterface_t* csma_ca_beb_interface;
extern const MacProtocolInterface_t* no_mac_interface;
extern osEventFlagsId_t channel_report_flag;

static const MacProtocolInterface_t* protocol_registry[NUM_MAC_PROTOCOL];

extern osMessageQueueId_t channel_report_queue;

/* Private function prototypes -----------------------------------------------*/

static void registerProtocols();

static void registerMacParams();
static void createTxQueues();
static void createRxQueues();
static void createHostStateMutex();

static void switchMacProtocol();
static void handleHostScheduledMessage();
static void resetTask();

static void clearScheduledHostMessageLocked(void);
static uint32_t hostLeadTimeCycles(uint32_t ms);

static void HostControl_Init(void* protocol_data);
static void HostControl_Deinit(void* protocol_data);
static MacState_t HostControl_HandleTxRequest(void* protocol_data);
static void HostControl_ProcessChannelReport(void* protocol_data, const ChannelReport_t report);
static MacState_t HostControl_ProcessRxMessage(void* protocol_data, const Message_t* message);
static MacState_t HostControl_EmergencyTx(void* protocol_data, const Message_t* message);

/* Exported function definitions ---------------------------------------------*/

void MAC_StartTask(void* argument)
{
  (void) (argument);
  registerProtocols();

  Error_RegisterTask("DAC");
  registerMacParams();
  Error_ParameterRegistrationComplete();

  createRxQueues();
  createTxQueues();
  createHostStateMutex();

  CFG_WaitLoadComplete();

  resetTask();

  for (;;) {
    switchMacProtocol();
    handleHostScheduledMessage();

    ChannelReport_t channel_report;
    if (osMessageQueueGet(channel_report_queue, &channel_report, NULL, 0) == osOK) {
      if (task_context.interface->processChannelReport != NULL) {
        task_context.interface->processChannelReport(&task_context.protocol_data.csma_ca_beb_data, channel_report);
      }
    }

    if (osMessageQueueGetCount(regular_tx_queue) != 0) {
      if (task_context.interface->handleTxRequest != NULL) {
        task_context.state = task_context.interface->handleTxRequest(&task_context.protocol_data.csma_ca_beb_data);
        switch (task_context.state) {
          case MAC_STATE_DROPPED:
            osEventFlagsSet(print_event_handle, MESS_MAC_DROPPED_MESSAGE);
            break;
          case MAC_STATE_ERROR:
            break;
          case MAC_STATE_SUCCESS: {
            // add to mess queue
            Message_t message;
            if (osMessageQueueGet(regular_tx_queue, &message, NULL, 0) != osOK) {
              osEventFlagsSet(print_event_handle, MESS_MAC_LOST_MESSAGE);
              break;
            }
            if (MESS_AddMessageToTxQ(&message) == false) {
              osEventFlagsSet(print_event_handle, MESS_MAC_TX_SPACE);
            }
            break;
          }
          case MAC_STATE_DEFERRED:
            break;
          default:
            break;
        }
      }
    }

    if (osMessageQueueGetCount(emergency_tx_queue) != 0) {
      if (task_context.interface->handleEmergencyTx != NULL) {
        Message_t message_to_send;
        osMessageQueueGet(emergency_tx_queue, &message_to_send, NULL, 0);
        task_context.state = task_context.interface->handleEmergencyTx(&task_context.protocol_data.csma_ca_beb_data, &message_to_send);
      }
    }

    if (osMessageQueueGetCount(mac_rx_queue) != 0) {
      Message_t received_message;
      osMessageQueueGet(mac_rx_queue, &received_message, NULL, 0);
      if (task_context.interface->processRxMessage != NULL) {
        task_context.state = task_context.interface->processRxMessage(&task_context.protocol_data.csma_ca_beb_data, &received_message);
      }

      MESS_AddMessageToRxQ(&received_message);
    }

    if (Error_CheckModuleReset() == TASK_RESET) {
      resetTask();
    }
    Error_ResetAbortFlag();

    osDelay(1);
  }
}

/* Private function definitions ----------------------------------------------*/

void registerProtocols()
{
  protocol_registry[MAC_PROTOCOL_NONE] = no_mac_interface;
  protocol_registry[MAC_PROTOCOL_CSMA_CA_BEB] = csma_ca_beb_interface;
  static const MacProtocolInterface_t host_control_interface = {
    .protocol = MAC_PROTOCOL_HOST_CONTROL,
    .init = HostControl_Init,
    .deinit = HostControl_Deinit,
    .handleTxRequest = HostControl_HandleTxRequest,
    .processChannelReport = HostControl_ProcessChannelReport,
    .processRxMessage = HostControl_ProcessRxMessage,
    .handleEmergencyTx = HostControl_EmergencyTx
  };
  protocol_registry[MAC_PROTOCOL_HOST_CONTROL] = &host_control_interface;
}

void registerMacParams()
{
  uint32_t min_u32 = MIN_MAC;
  uint32_t max_u32 = MAX_MAC;
  if (Param_Register(PARAM_MAC, "MAC method", PARAM_TYPE_ENUM, 
                     &task_context.requested_protocol, sizeof(MacProtocol_t),
                     &min_u32, &max_u32, NULL, mac_protocol_descriptions) == false) {
    REGISTER_ERROR(ERROR_PARAMETER_REGISTRATION);
  }
}

void createTxQueues()
{
  if (regular_tx_queue != NULL || emergency_tx_queue != NULL) 
    REGISTER_ERROR(ERROR_QUEUE_INITIALIZATION);
  
  regular_tx_queue = osMessageQueueNew(REGULAR_TX_QUEUE_SIZE, sizeof(Message_t), NULL);
  emergency_tx_queue = osMessageQueueNew(EMERGENCY_TX_QUEUE_SIZE, sizeof(Message_t), NULL);

  if (regular_tx_queue == NULL || emergency_tx_queue == NULL) 
    REGISTER_ERROR(ERROR_QUEUE_INITIALIZATION);
}

void createRxQueues()
{
  if (mac_rx_queue != NULL) 
    REGISTER_ERROR(ERROR_QUEUE_INITIALIZATION);
  
  mac_rx_queue = osMessageQueueNew(RX_QUEUE_SIZE, sizeof(Message_t), NULL);

  if (mac_rx_queue == NULL) 
    REGISTER_ERROR(ERROR_QUEUE_INITIALIZATION);
}

void createHostStateMutex()
{
  if (host_state_mutex != NULL) {
    REGISTER_ERROR(ERROR_MUTEX_INITIALIZATION);
  }

  host_state_mutex = osMutexNew(NULL);
  if (host_state_mutex == NULL) {
    REGISTER_ERROR(ERROR_MUTEX_INITIALIZATION);
  }
}

void switchMacProtocol()
{
  RETURN_IF_ERROR_PRESENT();
  if (task_context.current_protocol == task_context.requested_protocol) 
    return;

  if (task_context.current_protocol != MAC_PROTOCOL_UNKNOWN) {
    if (task_context.interface == NULL) 
      REGISTER_ERROR(ERROR_NULL_PTR);
    
    if (task_context.interface->deinit == NULL) 
      REGISTER_ERROR(ERROR_NULL_PTR);
    
    task_context.interface->deinit(&task_context.protocol_data);
  }

  for (uint16_t i = 0; i < sizeof(protocol_registry) / sizeof(protocol_registry[0]); i++) {
    if (protocol_registry[i]->protocol != task_context.requested_protocol) continue;
    
    protocol_registry[i]->init(&task_context.protocol_data);
    task_context.state = MAC_STATE_IDLE;
    task_context.interface = protocol_registry[i];
    task_context.current_protocol = protocol_registry[i]->protocol;
    return;
  }

  // Can only get here if protocol not in registry
  REGISTER_ERROR(ERROR_UNHANDLED_CASE);
}

void resetTask()
{
  // TODO: de-init and re-init mac method if known
}

bool MAC_IsHostModeEnabled(void)
{
  return task_context.requested_protocol == MAC_PROTOCOL_HOST_CONTROL;
}

bool MAC_ScheduleHostMessage(const Message_t* message, uint32_t* request_id)
{
  if (message == NULL || request_id == NULL) {
    return false;
  }

  if (MAC_IsHostModeEnabled() == false || message->delay == false) {
    return false;
  }

  uint32_t now_cyccnt = DWT->CYCCNT;
  uint32_t delta = message->delay_cyccnt - now_cyccnt;
  if (delta > INT32_MAX || delta <= hostLeadTimeCycles(HOST_TX_MIN_LEAD_MS)) {
    return false;
  }

  if (osMutexAcquire(host_state_mutex, osWaitForever) != osOK) {
    return false;
  }

  if (host_scheduled_message.pending == true) {
    osMutexRelease(host_state_mutex);
    return false;
  }

  memset(&host_scheduled_message, 0, sizeof(host_scheduled_message));
  host_scheduled_message.pending = true;
  host_scheduled_message.request_id = next_host_request_id++;
  if (next_host_request_id == 0) {
    next_host_request_id = 1;
  }
  memcpy(&host_scheduled_message.message, message, sizeof(host_scheduled_message.message));
  *request_id = host_scheduled_message.request_id;

  osMutexRelease(host_state_mutex);
  return true;
}

bool MAC_CancelHostMessage(uint32_t request_id)
{
  if (request_id == 0 || host_state_mutex == NULL) {
    return false;
  }

  if (osMutexAcquire(host_state_mutex, osWaitForever) != osOK) {
    return false;
  }

  bool cancelled = false;
  if (host_scheduled_message.pending == true &&
      host_scheduled_message.request_id == request_id) {
    clearScheduledHostMessageLocked();
    cancelled = true;
  }

  osMutexRelease(host_state_mutex);
  return cancelled;
}

bool MAC_GetHostStatus(MacHostStatus_t* status)
{
  if (status == NULL) {
    return false;
  }

  memset(status, 0, sizeof(*status));
  status->host_mode_enabled = MAC_IsHostModeEnabled();
  status->regular_tx_depth = (regular_tx_queue != NULL) ? osMessageQueueGetCount(regular_tx_queue) : 0;
  status->emergency_tx_depth = (emergency_tx_queue != NULL) ? osMessageQueueGetCount(emergency_tx_queue) : 0;

  if (host_state_mutex == NULL) {
    return true;
  }

  if (osMutexAcquire(host_state_mutex, osWaitForever) != osOK) {
    return false;
  }

  status->scheduled_tx_pending = host_scheduled_message.pending;
  status->scheduled_tx_id = host_scheduled_message.request_id;
  status->scheduled_tx_cyccnt = host_scheduled_message.message.delay_cyccnt;

  osMutexRelease(host_state_mutex);
  return true;
}

void handleHostScheduledMessage()
{
  if (task_context.current_protocol != MAC_PROTOCOL_HOST_CONTROL || host_state_mutex == NULL) {
    return;
  }

  if (osMutexAcquire(host_state_mutex, osWaitForever) != osOK) {
    return;
  }

  if (host_scheduled_message.pending == false) {
    osMutexRelease(host_state_mutex);
    return;
  }

  Message_t pending_message;
  memcpy(&pending_message, &host_scheduled_message.message, sizeof(pending_message));
  osMutexRelease(host_state_mutex);

  uint32_t current_cyccnt = DWT->CYCCNT;
  uint32_t delta = pending_message.delay_cyccnt - current_cyccnt;
  if (delta > INT32_MAX) {
    if (osMutexAcquire(host_state_mutex, osWaitForever) == osOK) {
      clearScheduledHostMessageLocked();
      osMutexRelease(host_state_mutex);
    }
    return;
  }

  if (delta > hostLeadTimeCycles(HOST_TX_HANDOFF_LEAD_MS)) {
    return;
  }

  if (MESS_AddMessageToTxQ(&pending_message) == true) {
    if (osMutexAcquire(host_state_mutex, osWaitForever) == osOK) {
      clearScheduledHostMessageLocked();
      osMutexRelease(host_state_mutex);
    }
    return;
  }

  if (delta <= hostLeadTimeCycles(HOST_TX_MIN_LEAD_MS)) {
    if (osMutexAcquire(host_state_mutex, osWaitForever) == osOK) {
      clearScheduledHostMessageLocked();
      osMutexRelease(host_state_mutex);
    }
    osEventFlagsSet(print_event_handle, MESS_MAC_TX_SPACE);
  }
}

void clearScheduledHostMessageLocked(void)
{
  memset(&host_scheduled_message, 0, sizeof(host_scheduled_message));
}

uint32_t hostLeadTimeCycles(uint32_t ms)
{
  return ms * (SystemCoreClock / 1000U);
}

void HostControl_Init(void* protocol_data)
{
  (void) protocol_data;

  if (osMutexAcquire(host_state_mutex, osWaitForever) == osOK) {
    clearScheduledHostMessageLocked();
    osMutexRelease(host_state_mutex);
  }

  while (channel_report_flag == NULL) {
    osDelay(1);
  }
  osEventFlagsSet(channel_report_flag, REPORT_16_CD_PSD);
}

void HostControl_Deinit(void* protocol_data)
{
  (void) protocol_data;

  if (osMutexAcquire(host_state_mutex, osWaitForever) == osOK) {
    clearScheduledHostMessageLocked();
    osMutexRelease(host_state_mutex);
  }

  osEventFlagsSet(channel_report_flag, REPORT_NONE);
}

MacState_t HostControl_HandleTxRequest(void* protocol_data)
{
  (void) protocol_data;
  return MAC_STATE_SUCCESS;
}

void HostControl_ProcessChannelReport(void* protocol_data, const ChannelReport_t report)
{
  (void) protocol_data;
  COMM_ReportHostSenseEvent(&report);
}

MacState_t HostControl_ProcessRxMessage(void* protocol_data, const Message_t* message)
{
  (void) protocol_data;
  (void) message;
  return MAC_STATE_SUCCESS;
}

MacState_t HostControl_EmergencyTx(void* protocol_data, const Message_t* message)
{
  (void) protocol_data;

  if (MESS_PriorityTransmission(message) == false) {
    return MAC_STATE_ERROR;
  }

  return MAC_STATE_SUCCESS;
}
