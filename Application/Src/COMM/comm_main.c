/*
 * comm_main.c
 *
 *  Created on: Feb 2, 2025
 *      Author: ericv
 * 
 * Copyright (c) 2025 OpenAquatix Contributors
 * SPDX-License-Identifier: MIT
 */

/* Private includes ----------------------------------------------------------*/

#include "stm32h7xx_hal.h"
#include "cmsis_os.h"
#include "arm_math.h"

#include "usb_comm.h"
#include "dau_card-driver.h"
#include "error_manager.h"

#include "comm_commands.h"
#include "comm_menu_registration.h"
#include "comm_main.h"
#include "comm_menu_system.h"
#include "comm_print.h"

#include "mess_main.h"
#include "base64.h"

#include "sys_pressure.h"
#include "sys_power.h"
#include "sys_temperature.h"

#include "cfg_main.h"
#include "cfg_parameters.h"
#include "cfg_defaults.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private typedef -----------------------------------------------------------*/

typedef struct {
  MenuNode_t* current_menu;
  CommInterface_t interface;
} MenuContext_t;

typedef struct {
  bool tag_mode;
  bool prompt_enabled;
  HmiInputContext_t input_context;
  bool prompt_active;
} HmiSession_t;

typedef struct {
  bool enabled;
  CommInterface_t interface;
} HostStreamSubscription_t;

typedef struct {
  uint64_t tick_ms;
  uint32_t cyccnt;
  bool temp_ready;
  bool power_ready;
  bool electrical_ready;
  bool env_ready;
  float tj_current_c;
  float tj_peak_c;
  float tj_avg_c;
  float power_latest_w;
  float power_peak_w;
  float power_avg_w;
  float energy_since_boot_j;
  float voltage_latest_v;
  float voltage_min_v;
  float voltage_max_v;
  float voltage_avg_v;
  float current_latest_a;
  float current_min_a;
  float current_max_a;
  float current_avg_a;
  float ambient_temp_c;
  float pressure_hpa;
} TelemetrySnapshot_t;

/* Private define ------------------------------------------------------------*/

#define ECHO_USB
// #define ECHO_UART

#define MAX_MENU_NUMBER_LENGTH    2
#define BUFFER_BACK_TRACK_AMOUNT  5
#define COMM_EVENT_TELEMETRY      (1UL << 0)

/* Private macro -------------------------------------------------------------*/

#define MIN(a, b)                 (((a) < (b)) ? (a) : (b))
#define MAX(a, b)                 (((a) > (b)) ? (a) : (b))

/* Private variables ---------------------------------------------------------*/

static MenuContext_t menu_context;
static HmiSession_t hmi_session = {
  .tag_mode = false,
  .prompt_enabled = false,
  .input_context = HMI_INPUT_CONTEXT_MENU,
  .prompt_active = false
};
static uint8_t out_buffer[MAX_COMM_OUT_BUFFER_SIZE];

static uint8_t msg_buffer[MAX_COMM_IN_BUFFER_SIZE];
static uint16_t msg_buf_len = 0;
static uint8_t test_msg[] = "Welcome to the UAM HMI!\r\n";

static uint16_t last_echo_len = 0;
static HostStreamSubscription_t host_rx_subscription = {
  .enabled = false,
  .interface = COMM_USB
};
static HostStreamSubscription_t host_sense_subscription = {
  .enabled = false,
  .interface = COMM_USB
};
static TelemetrySubscriptionStatus_t telemetry_subscription = {
  .enabled = false,
  .group = TELEMETRY_GROUP_NONE,
  .period_ms = 0,
  .interface = COMM_USB
};
static osEventFlagsId_t comm_event_handle = NULL;
static osTimerId_t telemetry_timer = NULL;

/* Private function prototypes -----------------------------------------------*/

static void registerMenus(void);
static RxState_t getHmiInput(CommInterface_t* interface);

static void handleHmiWithdraw(void);
static void handleHmiNavigation(void);
static void handleHmiFunction(void);
static void invokeCurrentLeafHandler(bool consume_input);

static void echoInput(void);

static void displaySubMenus(void);
static bool isNumber(uint8_t* buf, uint16_t len);
static bool checkMenuNumberInput(uint8_t* buf, uint16_t len, uint16_t* number);
static void updateInputEcho(uint8_t* msg_buffer, uint16_t len);
static void resetInputEcho(void);
static const char* getHmiTagString(HmiTag_t tag);
static void transmitHmiLineInternal(HmiTag_t tag, const char* text,
                                    CommInterface_t interface, bool force_tag);
static void transmitHmiPromptInternal(const char* text, CommInterface_t interface);
static void transmitFormattedHmiLine(HmiTag_t tag, CommInterface_t interface,
                                     bool force_tag, const char* format, va_list args);
static void finalizePromptLineIfNeeded(CommInterface_t interface);
static size_t appendMachineToken(char* buffer, size_t buffer_size, size_t offset,
                                 const char* format, ...);
static const char* messageRouteString(MessageType_t type);
static const char* protocolString(MessagingProtocol_t protocol);
static const char* customTypeString(CustomMessageData_t data_type);
static const char* janusTypeString(JanusMessageData_t data_type);
static void appendPreambleFieldToken(char* buffer, size_t buffer_size, size_t* offset,
                                     const char* key, PreambleValue_t value);
static void initializeTelemetryResources(void);
static void telemetryTimerCallback(void* argument);
static const char* telemetryGroupString(TelemetryGroup_t group);
static bool collectTelemetrySnapshot(TelemetrySnapshot_t* snapshot);
static size_t appendTelemetryGroupFields(char* buffer, size_t buffer_size, size_t offset,
                                         TelemetryGroup_t group,
                                         const TelemetrySnapshot_t* snapshot);
static bool buildTelemetryLine(char* buffer, size_t buffer_size, const char* prefix,
                               TelemetryGroup_t group, const TelemetrySnapshot_t* snapshot);
static void processTelemetryStreamEvent(void);

static void printNotifications(void);

static void registerCommParams(void);

static void resetTask(void);

/* Exported function definitions ---------------------------------------------*/

void COMM_StartTask(void *argument)
{
  (void)(argument);
  USB_CreateShared();
  menu_context.interface = COMM_BOTH;

  Error_RegisterTask("COMM");
  registerCommParams();
  Error_ParameterRegistrationComplete();
  CFG_WaitLoadComplete();
  initializeTelemetryResources();

  osDelay(1000);

  COMM_TransmitData(test_msg, sizeof(test_msg) - 1, menu_context.interface);

  registerMenus();

  menu_context.current_menu = MenuSystem_GetMenu(MENU_ID_MAIN);
  displaySubMenus();
  resetTask();
  for(;;) {
    Message_t rx_msg;
    if (MESS_GetMessageFromRxQ(&rx_msg) == true) {
      COMM_ReportHostRxEvent(&rx_msg);
      Print_DisplayReceivedMessage(&rx_msg, out_buffer, menu_context.interface);
    }

    RxState_t state = getHmiInput(&menu_context.interface);

    printNotifications();
    processTelemetryStreamEvent();

    switch (state) {
      case DATA_READY:
        if (msg_buffer[0] == WITHDRAW_CHAR && msg_buf_len > 0) {
          resetInputEcho();
          handleHmiWithdraw();
          break;
        }

        if (menu_context.current_menu->num_children != 0) {
          resetInputEcho();
          hmi_session.input_context = HMI_INPUT_CONTEXT_MENU;
          CommandContext_t command_context = {
            .interface = menu_context.interface,
            .current_menu = &menu_context.current_menu,
            .redraw_menu = false
          };

          if (COMM_Commands_Process((char*) msg_buffer, &command_context) == false) {
            handleHmiNavigation();
          }
          else if (command_context.redraw_menu == true) {
            displaySubMenus();
          }
        }
        else {
          hmi_session.input_context = HMI_INPUT_CONTEXT_FUNCTION;
          handleHmiFunction();
        }
        break;
      case NEW_CONTENT:
        hmi_session.input_context = (menu_context.current_menu->num_children == 0) ?
            HMI_INPUT_CONTEXT_FUNCTION : HMI_INPUT_CONTEXT_MENU;
        echoInput();
        break;
      case NO_CHANGE:
        break;
      default:
        break;
    }

    if (Error_CheckModuleReset() == TASK_RESET) {
      resetTask();
    }
    Error_ResetAbortFlag();
    osDelay(10);
  }
}


void COMM_TransmitData(const void *data, uint32_t data_len, CommInterface_t interface)
{
  if (data_len == CALC_LEN) {
    data_len = strlen((char*) data);
  }
  switch (interface) {
    case COMM_USB:
      USB_TransmitData((uint8_t*) data, (uint16_t) data_len);
      break;
    case COMM_UART:
      DAU_TransmitData((uint8_t*) data, (uint16_t) data_len);
      break;
    case COMM_BOTH:
      USB_TransmitData((uint8_t*) data, (uint16_t) data_len);
      DAU_TransmitData((uint8_t*) data, (uint16_t) data_len);
      break;
    default:
      break;
  }
}

bool COMM_IsTagModeEnabled(void)
{
  return hmi_session.tag_mode;
}

void COMM_SetTagMode(bool enabled)
{
  hmi_session.tag_mode = enabled;
  if (enabled == false) {
    hmi_session.prompt_active = false;
  }
}

bool COMM_IsPromptEnabled(void)
{
  return hmi_session.prompt_enabled;
}

void COMM_SetPromptEnabled(bool enabled)
{
  hmi_session.prompt_enabled = enabled;
  if (enabled == false) {
    hmi_session.prompt_active = false;
  }
}

void COMM_TransmitHmiLine(HmiTag_t tag, const char* text, CommInterface_t interface)
{
  transmitHmiLineInternal(tag, text, interface, false);
}

void COMM_TransmitHmiLinef(HmiTag_t tag, CommInterface_t interface, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  transmitFormattedHmiLine(tag, interface, false, format, args);
  va_end(args);
}

void COMM_TransmitHmiMachineLine(HmiTag_t tag, const char* text, CommInterface_t interface)
{
  transmitHmiLineInternal(tag, text, interface, true);
}

void COMM_TransmitHmiMachineLinef(HmiTag_t tag, CommInterface_t interface, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  transmitFormattedHmiLine(tag, interface, true, format, args);
  va_end(args);
}

void COMM_TransmitTaggedText(HmiTag_t tag, const char* text, CommInterface_t interface)
{
  char line_buffer[MAX_COMM_OUT_BUFFER_SIZE];
  const char* cursor = (text != NULL) ? text : "";

  if (COMM_IsTagModeEnabled() == false) {
    COMM_TransmitData(cursor, CALC_LEN, interface);
    return;
  }

  while (*cursor != '\0') {
    while (*cursor == '\r' || *cursor == '\n') {
      cursor++;
    }

    if (*cursor == '\0') {
      break;
    }

    size_t line_length = 0;
    while (cursor[line_length] != '\0' &&
           cursor[line_length] != '\r' &&
           cursor[line_length] != '\n') {
      line_length++;
    }

    size_t copy_length = MIN(line_length, sizeof(line_buffer) - 1);
    memcpy(line_buffer, cursor, copy_length);
    line_buffer[copy_length] = '\0';
    COMM_TransmitHmiLine(tag, line_buffer, interface);

    cursor += line_length;
  }
}

void COMM_TransmitTaggedTextf(HmiTag_t tag, CommInterface_t interface, const char* format, ...)
{
  char buffer[MAX_COMM_OUT_BUFFER_SIZE];
  va_list args;

  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  COMM_TransmitTaggedText(tag, buffer, interface);
}

void COMM_TransmitHmiPrompt(const char* text, CommInterface_t interface)
{
  transmitHmiPromptInternal(text, interface);
}

void COMM_TransmitHmiPromptf(CommInterface_t interface, const char* format, ...)
{
  char buffer[MAX_COMM_OUT_BUFFER_SIZE];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  transmitHmiPromptInternal(buffer, interface);
}

void COMM_SetHostRxSubscription(bool enabled, CommInterface_t interface)
{
  host_rx_subscription.enabled = enabled;
  if (enabled == true) {
    host_rx_subscription.interface = interface;
  }
}

void COMM_SetHostSenseSubscription(bool enabled, CommInterface_t interface)
{
  host_sense_subscription.enabled = enabled;
  if (enabled == true) {
    host_sense_subscription.interface = interface;
  }
}

bool COMM_IsHostRxSubscriptionEnabled(void)
{
  return host_rx_subscription.enabled;
}

bool COMM_IsHostSenseSubscriptionEnabled(void)
{
  return host_sense_subscription.enabled;
}

bool COMM_TransmitTelemetrySnapshot(CommInterface_t interface, TelemetryGroup_t group)
{
  TelemetrySnapshot_t snapshot;
  char event_buffer[MAX_COMM_OUT_BUFFER_SIZE];

  if (group == TELEMETRY_GROUP_NONE) {
    return false;
  }

  if (collectTelemetrySnapshot(&snapshot) == false ||
      buildTelemetryLine(event_buffer, sizeof(event_buffer), "OK :telemetry", group,
                         &snapshot) == false) {
    return false;
  }

  COMM_TransmitHmiMachineLine(HMI_TAG_STATUS, event_buffer, interface);
  return true;
}

bool COMM_SetHostTelemetrySubscription(bool enabled, TelemetryGroup_t group, uint32_t period_ms,
                                       CommInterface_t interface)
{
  if (comm_event_handle == NULL || telemetry_timer == NULL) {
    return false;
  }

  if (enabled == false) {
    if (telemetry_subscription.enabled == true && osTimerStop(telemetry_timer) != osOK) {
      return false;
    }

    telemetry_subscription.enabled = false;
    telemetry_subscription.group = TELEMETRY_GROUP_NONE;
    telemetry_subscription.period_ms = 0;
    osEventFlagsClear(comm_event_handle, COMM_EVENT_TELEMETRY);
    return true;
  }

  if (group == TELEMETRY_GROUP_NONE || period_ms == 0) {
    return false;
  }

  if (osTimerStop(telemetry_timer) != osOK && telemetry_subscription.enabled == true) {
    return false;
  }

  telemetry_subscription.enabled = true;
  telemetry_subscription.group = group;
  telemetry_subscription.period_ms = period_ms;
  telemetry_subscription.interface = interface;

  if (osTimerStart(telemetry_timer, period_ms) != osOK) {
    telemetry_subscription.enabled = false;
    telemetry_subscription.group = TELEMETRY_GROUP_NONE;
    telemetry_subscription.period_ms = 0;
    return false;
  }

  return true;
}

bool COMM_GetHostTelemetrySubscriptionStatus(TelemetrySubscriptionStatus_t* status)
{
  if (status == NULL) {
    return false;
  }

  *status = telemetry_subscription;
  return true;
}

void COMM_ReportHostRxEvent(const Message_t* msg)
{
  if (msg == NULL || host_rx_subscription.enabled == false) {
    return;
  }

  char event_buffer[MAX_COMM_OUT_BUFFER_SIZE];
  size_t offset = 0;

  offset = appendMachineToken(event_buffer, sizeof(event_buffer), offset,
      "EVENT protocol=%s route=%s timestamp_ms=%lu rx_cyccnt=%lu length_bits=%u error=%u",
      protocolString(msg->protocol), messageRouteString(msg->type),
      (unsigned long) msg->timestamp, (unsigned long) msg->rx_cyccnt,
      msg->length_bits, msg->error_detected ? 1U : 0U);
  offset = appendMachineToken(event_buffer, sizeof(event_buffer), offset,
      " snr=%.3f doppler_mps=%.3f", (double) msg->snr, (double) msg->doppler_mps);

  if (msg->protocol == PROTOCOL_CUSTOM) {
    offset = appendMachineToken(event_buffer, sizeof(event_buffer), offset,
        " data_type=%s", customTypeString(msg->data_type));
  }
  else if (msg->protocol == PROTOCOL_JANUS) {
    offset = appendMachineToken(event_buffer, sizeof(event_buffer), offset,
        " janus_type=%s", janusTypeString(msg->janus_data_type));
  }

  appendPreambleFieldToken(event_buffer, sizeof(event_buffer), &offset, "modem_id",
                           msg->preamble.modem_id);
  appendPreambleFieldToken(event_buffer, sizeof(event_buffer), &offset, "is_mobile",
                           msg->preamble.is_mobile);
  appendPreambleFieldToken(event_buffer, sizeof(event_buffer), &offset, "reservation_time_10ms",
                           msg->preamble.reservation_time_10ms);
  appendPreambleFieldToken(event_buffer, sizeof(event_buffer), &offset, "schedule_flag",
                           msg->preamble.schedule_flag);
  appendPreambleFieldToken(event_buffer, sizeof(event_buffer), &offset, "destination_id",
                           msg->preamble.destination_id);
  appendPreambleFieldToken(event_buffer, sizeof(event_buffer), &offset, "tx_rx_capable",
                           msg->preamble.tx_rx_capable);
  appendPreambleFieldToken(event_buffer, sizeof(event_buffer), &offset, "can_forward",
                           msg->preamble.can_forward);
  appendPreambleFieldToken(event_buffer, sizeof(event_buffer), &offset, "coding",
                           msg->preamble.coding);
  appendPreambleFieldToken(event_buffer, sizeof(event_buffer), &offset, "encryption",
                           msg->preamble.encryption);

  if (msg->data_type == RANGING_RESPONSE) {
    offset = appendMachineToken(event_buffer, sizeof(event_buffer), offset,
        " range_m=%.3f", (double) msg->range_m);
  }

  uint16_t payload_len_bytes = (msg->length_bits + 7U) / 8U;
  if (payload_len_bytes > PACKET_DATA_MAX_LENGTH_BYTES) {
    payload_len_bytes = PACKET_DATA_MAX_LENGTH_BYTES;
  }

  if (payload_len_bytes > 0) {
    size_t encoded_length = 0;
    unsigned char* encoded_payload = base64_encode(msg->data, payload_len_bytes, &encoded_length);
    if (encoded_payload != NULL) {
      offset = appendMachineToken(event_buffer, sizeof(event_buffer), offset,
          " payload_b64=%s", (char*) encoded_payload);
      free(encoded_payload);
    }
  }

  COMM_TransmitHmiMachineLine(HMI_TAG_MSG_RX, event_buffer, host_rx_subscription.interface);
}

void COMM_ReportHostSenseEvent(const ChannelReport_t* report)
{
  if (report == NULL || host_sense_subscription.enabled == false) {
    return;
  }

  COMM_TransmitHmiMachineLinef(HMI_TAG_NOTIFY, host_sense_subscription.interface,
      "EVENT sense timestamp_ms=%llu cyccnt=%lu psd=%.6f",
      (unsigned long long) report->timestamp_ms, (unsigned long) report->cyccnt,
      (double) report->psd);
}

/* Private function definitions ----------------------------------------------*/

void initializeTelemetryResources(void)
{
  if (comm_event_handle == NULL) {
    comm_event_handle = osEventFlagsNew(NULL);
    if (comm_event_handle == NULL) {
      REGISTER_ERROR(ERROR_FLAGS_INITIALIZATION);
    }
  }

  if (telemetry_timer == NULL) {
    telemetry_timer = osTimerNew(telemetryTimerCallback, osTimerPeriodic, NULL, NULL);
    if (telemetry_timer == NULL) {
      REGISTER_ERROR(ERROR_TIMER_INITIALIZATION);
    }
  }
}

void telemetryTimerCallback(void* argument)
{
  (void) argument;

  if (comm_event_handle != NULL) {
    osEventFlagsSet(comm_event_handle, COMM_EVENT_TELEMETRY);
  }
}

const char* telemetryGroupString(TelemetryGroup_t group)
{
  switch (group) {
    case TELEMETRY_GROUP_ALL:
      return "all";
    case TELEMETRY_GROUP_TEMP:
      return "temp";
    case TELEMETRY_GROUP_POWER:
      return "power";
    case TELEMETRY_GROUP_ELECTRICAL:
      return "electrical";
    case TELEMETRY_GROUP_ENV:
      return "env";
    case TELEMETRY_GROUP_NONE:
    default:
      return "none";
  }
}

bool collectTelemetrySnapshot(TelemetrySnapshot_t* snapshot)
{
  if (snapshot == NULL) {
    return false;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->tick_ms = HAL_AbsoluteTimestamp();
  snapshot->cyccnt = DWT->CYCCNT;

  snapshot->temp_ready = Temperature_IsJunctionReady();
  snapshot->power_ready = Power_IsReady();
  snapshot->electrical_ready = Power_IsReady();
  snapshot->env_ready = Temperature_IsAmbientReady() && Pressure_IsReady();

  if (snapshot->temp_ready == true) {
    snapshot->tj_current_c = Temperature_GetCurrentTj();
    snapshot->tj_peak_c = Temperature_GetPeakTj();
    snapshot->tj_avg_c = Temperature_GetAverageTj();
  }

  if (snapshot->power_ready == true) {
    snapshot->power_latest_w = Power_LatestPower();
    snapshot->power_peak_w = Power_MaxPower();
    snapshot->power_avg_w = Power_AveragePower();
    snapshot->energy_since_boot_j =
        snapshot->power_avg_w * (((float) snapshot->tick_ms) / 1000.0f);
  }

  if (snapshot->electrical_ready == true) {
    snapshot->voltage_latest_v = Power_LatestVoltage();
    snapshot->voltage_min_v = Power_MinVoltage();
    snapshot->voltage_max_v = Power_MaxVoltage();
    snapshot->voltage_avg_v = Power_AverageVoltage();
    snapshot->current_latest_a = Power_LatestCurrent();
    snapshot->current_min_a = Power_MinCurrent();
    snapshot->current_max_a = Power_MaxCurrent();
    snapshot->current_avg_a = Power_AverageCurrent();
  }

  if (snapshot->env_ready == true) {
    snapshot->ambient_temp_c = Temperature_GetCurrentTa();
    snapshot->pressure_hpa = Pressure_GetCurrent();
  }

  return true;
}

size_t appendTelemetryGroupFields(char* buffer, size_t buffer_size, size_t offset,
                                  TelemetryGroup_t group, const TelemetrySnapshot_t* snapshot)
{
  if (buffer == NULL || snapshot == NULL) {
    return offset;
  }

  if ((group == TELEMETRY_GROUP_ALL || group == TELEMETRY_GROUP_TEMP) &&
      snapshot->temp_ready == true) {
    offset = appendMachineToken(buffer, buffer_size, offset,
        " tj_current_c=%.3f tj_peak_c=%.3f tj_avg_c=%.3f",
        (double) snapshot->tj_current_c, (double) snapshot->tj_peak_c,
        (double) snapshot->tj_avg_c);
  }

  if ((group == TELEMETRY_GROUP_ALL || group == TELEMETRY_GROUP_POWER) &&
      snapshot->power_ready == true) {
    offset = appendMachineToken(buffer, buffer_size, offset,
        " power_latest_w=%.3f power_peak_w=%.3f power_avg_w=%.3f energy_since_boot_j=%.3f",
        (double) snapshot->power_latest_w, (double) snapshot->power_peak_w,
        (double) snapshot->power_avg_w, (double) snapshot->energy_since_boot_j);
  }

  if ((group == TELEMETRY_GROUP_ALL || group == TELEMETRY_GROUP_ELECTRICAL) &&
      snapshot->electrical_ready == true) {
    offset = appendMachineToken(buffer, buffer_size, offset,
        " voltage_latest_v=%.3f voltage_min_v=%.3f voltage_max_v=%.3f voltage_avg_v=%.3f"
        " current_latest_a=%.3f current_min_a=%.3f current_max_a=%.3f current_avg_a=%.3f",
        (double) snapshot->voltage_latest_v, (double) snapshot->voltage_min_v,
        (double) snapshot->voltage_max_v, (double) snapshot->voltage_avg_v,
        (double) snapshot->current_latest_a, (double) snapshot->current_min_a,
        (double) snapshot->current_max_a, (double) snapshot->current_avg_a);
  }

  if ((group == TELEMETRY_GROUP_ALL || group == TELEMETRY_GROUP_ENV) &&
      snapshot->env_ready == true) {
    offset = appendMachineToken(buffer, buffer_size, offset,
        " ambient_temp_c=%.3f pressure_hpa=%.3f",
        (double) snapshot->ambient_temp_c, (double) snapshot->pressure_hpa);
  }

  return offset;
}

bool buildTelemetryLine(char* buffer, size_t buffer_size, const char* prefix,
                        TelemetryGroup_t group, const TelemetrySnapshot_t* snapshot)
{
  size_t offset = 0;

  if (buffer == NULL || buffer_size == 0 || prefix == NULL || snapshot == NULL ||
      group == TELEMETRY_GROUP_NONE) {
    return false;
  }

  offset = appendMachineToken(buffer, buffer_size, offset,
      "%s group=%s tick_ms=%llu cyccnt=%lu temp_ready=%s power_ready=%s electrical_ready=%s env_ready=%s",
      prefix, telemetryGroupString(group), (unsigned long long) snapshot->tick_ms,
      (unsigned long) snapshot->cyccnt, snapshot->temp_ready ? "yes" : "no",
      snapshot->power_ready ? "yes" : "no",
      snapshot->electrical_ready ? "yes" : "no",
      snapshot->env_ready ? "yes" : "no");
  offset = appendTelemetryGroupFields(buffer, buffer_size, offset, group, snapshot);
  (void) offset;
  return true;
}

void processTelemetryStreamEvent(void)
{
  if (comm_event_handle == NULL || telemetry_subscription.enabled == false) {
    return;
  }

  uint32_t flags = osEventFlagsGet(comm_event_handle);
  if ((flags & COMM_EVENT_TELEMETRY) == 0U) {
    return;
  }

  osEventFlagsClear(comm_event_handle, COMM_EVENT_TELEMETRY);

  TelemetrySnapshot_t snapshot;
  char event_buffer[MAX_COMM_OUT_BUFFER_SIZE];

  if (collectTelemetrySnapshot(&snapshot) == false ||
      buildTelemetryLine(event_buffer, sizeof(event_buffer), "EVENT telemetry",
                         telemetry_subscription.group, &snapshot) == false) {
    return;
  }

  COMM_TransmitHmiMachineLine(HMI_TAG_NOTIFY, event_buffer, telemetry_subscription.interface);
}

void registerMenus(void)
{
  COMM_RegisterMainMenu();
  COMM_RegisterConfigurationMenu();
  COMM_RegisterDebugMenu();
  COMM_RegisterHistoryMenu();
  COMM_RegisterTxRxMenu();
  COMM_RegisterEvalMenu();
  COMM_RegisterJanusMenu();
}

RxState_t getHmiInput(CommInterface_t* interface)
{
  // Check for HMI input on USB. If it exists, update state and set interface to USB
  RxState_t state = USB_GetHmiInput(msg_buffer, &msg_buf_len);
  if (state != NO_CHANGE) {
    *interface = COMM_USB;
    return state;
  }

  // Check for HMI input on UART. If it exists, update state and set interface to UART
  state = DAU_GetHmiInput(msg_buffer, &msg_buf_len);
  if (state != NO_CHANGE) {
    *interface = COMM_UART;
    return state;
  }
  return state; // NO_CHANGE
}

void handleHmiWithdraw(void)
{
  if (menu_context.current_menu->parameters != NULL) {
    menu_context.current_menu->parameters->state = PARAM_STATE_0;
  }
  menu_context.current_menu = MenuSystem_GetMenu(menu_context.current_menu->parent_id);
  displaySubMenus();
}

void handleHmiNavigation(void)
{
  // Cannot navigate without children
  if (menu_context.current_menu->num_children == 0) return;

  uint16_t menu_number;
  if (checkMenuNumberInput(msg_buffer, msg_buf_len, &menu_number) == true) {
    // Valid menu option
    menu_context.current_menu = MenuSystem_GetMenu(menu_context.current_menu->children_ids[menu_number - 1]);
    if (menu_context.current_menu->parameters != NULL) {
      menu_context.current_menu->parameters->state = PARAM_STATE_0;
    }

    if (menu_context.current_menu->num_children == 0) {
      hmi_session.input_context = HMI_INPUT_CONTEXT_FUNCTION;
      invokeCurrentLeafHandler(false);

      if (menu_context.current_menu->parameters->state == PARAM_STATE_COMPLETE) {
        menu_context.current_menu->parameters->state = PARAM_STATE_0;
        menu_context.current_menu = MenuSystem_GetMenu(menu_context.current_menu->parent_id);
        displaySubMenus();
      }
      return;
    }
  }
  else {
    if (COMM_IsTagModeEnabled() == true) {
      COMM_TransmitHmiLine(HMI_TAG_ERROR, "Invalid option!", menu_context.interface);
    }
    else {
      COMM_TransmitData("\r\nInvalid option!\r\n", CALC_LEN, menu_context.interface);
    }
  }
  displaySubMenus();
}

void handleHmiFunction(void)
{
  if (menu_context.current_menu->num_children != 0) return;

  invokeCurrentLeafHandler(true);

  if (menu_context.current_menu->parameters->state == PARAM_STATE_COMPLETE) {
    menu_context.current_menu->parameters->state = PARAM_STATE_0;
    menu_context.current_menu = MenuSystem_GetMenu(menu_context.current_menu->parent_id);
    displaySubMenus();
  }
}

static void invokeCurrentLeafHandler(bool consume_input)
{
  if (menu_context.current_menu->num_children != 0) return;

  // no children so handle function
  // Prepare function argument
  if (consume_input == true) {
    updateInputEcho(msg_buffer, msg_buf_len);
    osDelay(1);
    resetInputEcho();
  }

  FunctionContext_t context = {
      .state = menu_context.current_menu->parameters,
      .input_len = consume_input ? msg_buf_len : 0,
      .output_buffer = msg_buffer,
      .comm_interface = menu_context.interface
  };
  if (consume_input == true) {
    strncpy(context.input, (char*) msg_buffer, MAX_COMM_IN_BUFFER_SIZE);
  }
  else {
    context.input[0] = '\0';
  }

  (*menu_context.current_menu->handler)(&context);
  resetInputEcho();
}

void echoInput(void)
{
  #ifdef ECHO_USB
    if (menu_context.interface == COMM_USB) {
      updateInputEcho(msg_buffer, msg_buf_len);
    }
  #endif
  #ifdef ECHO_UART
    if (menu_context.interface == COMM_UART) {
      updateInputEcho(msg_buffer, msg_buf_len);
    }
  #endif
}

void displaySubMenus(void)
{
  if (menu_context.current_menu->num_children == 0) {
    hmi_session.input_context = HMI_INPUT_CONTEXT_FUNCTION;
    return;
  }

  hmi_session.input_context = HMI_INPUT_CONTEXT_MENU;
  if (COMM_IsTagModeEnabled() == true) {
    COMM_TransmitHmiLine(HMI_TAG_MENU, menu_context.current_menu->description,
                         menu_context.interface);
    for (int i = 0; i < menu_context.current_menu->num_children; i++) {
      uint16_t child_id = menu_context.current_menu->children_ids[i];
      MenuNode_t* child_menu = MenuSystem_GetMenu(child_id);
      snprintf((char*) out_buffer, sizeof(out_buffer), "%d: %s", i + 1,
               child_menu->description);
      COMM_TransmitHmiLine(HMI_TAG_MENU, (char*) out_buffer, menu_context.interface);
    }
    if (COMM_IsPromptEnabled() == true) {
      COMM_TransmitHmiPrompt("Select option", menu_context.interface);
    }
    return;
  }

  COMM_TransmitData("\r\n", 2, menu_context.interface);
  COMM_TransmitData(menu_context.current_menu->description, CALC_LEN, menu_context.interface);
  COMM_TransmitData("\r\n", 2, menu_context.interface);
  for (int i = 0; i < menu_context.current_menu->num_children; i++) {
    uint16_t child_id = menu_context.current_menu->children_ids[i];
    // TODO: add error checking
    MenuNode_t* child_menu = MenuSystem_GetMenu(child_id);
    sprintf((char*) out_buffer, "%d: %s\r\n", i + 1, child_menu->description);
    COMM_TransmitData(out_buffer, CALC_LEN, menu_context.interface);
  }
}

bool isNumber(uint8_t* buf, uint16_t len)
{
  for (int i = 0; i < len; i++) {
    if (! isdigit(buf[i])) return false;
  }
  return true;
}

bool checkMenuNumberInput(uint8_t* buf, uint16_t len, uint16_t* number)
{
  if (! isNumber(buf, len)) return false;
  if (len > MAX_MENU_NUMBER_LENGTH) return false;

  *number = (uint16_t) atoi((char*)buf);
  uint16_t num_children = menu_context.current_menu->num_children;

  if (*number > num_children) return false;
  return true;
}

void updateInputEcho(uint8_t* msg_buffer, uint16_t len)
{
  int16_t len_difference = (int16_t) len - (int16_t) last_echo_len;
  uint16_t start_index;
  if (len_difference > BUFFER_BACK_TRACK_AMOUNT) {
    start_index = last_echo_len;
  }
  else {
    start_index = (len > BUFFER_BACK_TRACK_AMOUNT) ? (len - BUFFER_BACK_TRACK_AMOUNT) : 0;
  }
  uint16_t back_amount = (uint16_t) MIN((int16_t) last_echo_len,
      (int16_t) (BUFFER_BACK_TRACK_AMOUNT - MIN(len_difference, BUFFER_BACK_TRACK_AMOUNT)));
  uint16_t out_buffer_len = back_amount;
  if (back_amount > 0) {
    memset(out_buffer, '\b', back_amount);
  }
  uint16_t new_data_len = MIN(len, MAX(BUFFER_BACK_TRACK_AMOUNT, len_difference));
  for (int i = 0; i < new_data_len; i++) {
    out_buffer[back_amount + i] = msg_buffer[start_index + i];
    out_buffer_len++;
  }
  if (last_echo_len > len) {
    for (int i = 0; i < last_echo_len - len; i++) {
      out_buffer[out_buffer_len++] = ' ';
    }
    for (int i = 0; i < last_echo_len - len; i++) {
      out_buffer[out_buffer_len++] = '\b';
    }
  }

  COMM_TransmitData(out_buffer, MAX(out_buffer_len, len_difference), menu_context.interface);

  last_echo_len = len;
}

void resetInputEcho(void)
{
  last_echo_len = 0;
}

void printNotifications(void)
{
  if (print_event_handle == NULL) return;

  uint32_t flags = osEventFlagsGet(print_event_handle);
  if (flags & MESS_DROPPED_PACKET_PREAMBLE) {
    if (COMM_IsTagModeEnabled() == true) {
      COMM_TransmitHmiLine(HMI_TAG_NOTIFY,
          "Dropped a packet with an invalid preamble", menu_context.interface);
    }
    else {
      COMM_TransmitData("Dropped a packet with an invalid preamble\r\n", CALC_LEN,
                        menu_context.interface);
    }
    osEventFlagsClear(print_event_handle, MESS_DROPPED_PACKET_PREAMBLE);
  }
  if (flags & MESS_DROPPED_PACKET_CARGO) {
    if (COMM_IsTagModeEnabled() == true) {
      COMM_TransmitHmiLine(HMI_TAG_NOTIFY,
          "Dropped a packet with an invalid cargo", menu_context.interface);
    }
    else {
      COMM_TransmitData("Dropped a packet with an invalid cargo\r\n", CALC_LEN,
                        menu_context.interface);
    }
    osEventFlagsClear(print_event_handle, MESS_DROPPED_PACKET_CARGO);
  }
  if (flags & MESS_MAC_LOST_MESSAGE) {
    COMM_TransmitTaggedText(HMI_TAG_NOTIFY,
        "TX REQUEST FAILED: Lost message in MAC; message not sent\r\n",
        menu_context.interface);
    osEventFlagsClear(print_event_handle, MESS_MAC_LOST_MESSAGE);
  }
  if (flags & MESS_MAC_TX_SPACE) {
    COMM_TransmitTaggedText(HMI_TAG_NOTIFY,
        "TX REQUEST FAILED: No space in TX queue for message\r\n",
        menu_context.interface);
    osEventFlagsClear(print_event_handle, MESS_MAC_TX_SPACE);
  }
  if (flags & MESS_MAC_DROPPED_MESSAGE) {
    COMM_TransmitTaggedText(HMI_TAG_NOTIFY,
        "TX REQUEST FAILED: Channel did not free in time\r\n",
        menu_context.interface);
    osEventFlagsClear(print_event_handle, MESS_MAC_DROPPED_MESSAGE);
  }
  if (flags & MESS_FAILED_RANGING_REQUEST) {
    COMM_TransmitTaggedText(HMI_TAG_NOTIFY,
        "Failed to send ranging request\r\n", menu_context.interface);
    osEventFlagsClear(print_event_handle, MESS_FAILED_RANGING_REQUEST);
  }
  if (flags & MESS_FAILED_RANGING_RESPONSE) {
    COMM_TransmitTaggedText(HMI_TAG_NOTIFY,
        "Received ranging request, but could not respond\r\n",
        menu_context.interface);
    osEventFlagsClear(print_event_handle, MESS_FAILED_RANGING_RESPONSE);
  }
  if (flags & MESS_RECEIVED_RANGING_RESPONSE_BAD) {
    COMM_TransmitTaggedText(HMI_TAG_NOTIFY,
        "Received valid ranging response, but could not add to queue\r\n",
        menu_context.interface);
    osEventFlagsClear(print_event_handle, MESS_RECEIVED_RANGING_RESPONSE_BAD);
  }
}

void registerCommParams(void)
{
  Print_RegisterParams();
}

void resetTask(void)
{
  USB_Init();
  DAU_Init();
}

static const char* getHmiTagString(HmiTag_t tag)
{
  switch (tag) {
    case HMI_TAG_MENU:
      return "MENU";
    case HMI_TAG_PROMPT:
      return "PROMPT";
    case HMI_TAG_STATUS:
      return "STATUS";
    case HMI_TAG_ERROR:
      return "ERROR";
    case HMI_TAG_NOTIFY:
      return "NOTIFY";
    case HMI_TAG_MSG_RX:
      return "MSG_RX";
    default:
      return "STATUS";
  }
}

static void transmitHmiLineInternal(HmiTag_t tag, const char* text,
                                    CommInterface_t interface, bool force_tag)
{
  char line_buffer[MAX_COMM_OUT_BUFFER_SIZE];
  const char* safe_text = (text != NULL) ? text : "";

  if (force_tag == true || hmi_session.tag_mode == true) {
    finalizePromptLineIfNeeded(interface);
    snprintf(line_buffer, sizeof(line_buffer), "[%s] %s\r\n", getHmiTagString(tag),
             safe_text);
  }
  else {
    snprintf(line_buffer, sizeof(line_buffer), "%s\r\n", safe_text);
  }

  COMM_TransmitData(line_buffer, CALC_LEN, interface);
}

static void transmitHmiPromptInternal(const char* text, CommInterface_t interface)
{
  char prompt_buffer[MAX_COMM_OUT_BUFFER_SIZE];
  const char* safe_text = (text != NULL) ? text : "";

  if (hmi_session.prompt_enabled == false) {
    hmi_session.prompt_active = false;
    return;
  }

  if (hmi_session.tag_mode == true) {
    finalizePromptLineIfNeeded(interface);
    snprintf(prompt_buffer, sizeof(prompt_buffer), "[%s] %s > ",
             getHmiTagString(HMI_TAG_PROMPT), safe_text);
    hmi_session.prompt_active = true;
  }
  else {
    snprintf(prompt_buffer, sizeof(prompt_buffer), "%s\r\n", safe_text);
    hmi_session.prompt_active = false;
  }

  COMM_TransmitData(prompt_buffer, CALC_LEN, interface);
}

static void transmitFormattedHmiLine(HmiTag_t tag, CommInterface_t interface,
                                     bool force_tag, const char* format, va_list args)
{
  char text_buffer[MAX_COMM_OUT_BUFFER_SIZE];

  vsnprintf(text_buffer, sizeof(text_buffer), format, args);
  transmitHmiLineInternal(tag, text_buffer, interface, force_tag);
}

static void finalizePromptLineIfNeeded(CommInterface_t interface)
{
  if (hmi_session.prompt_active == true) {
    COMM_TransmitData("\r\n", 2, interface);
    hmi_session.prompt_active = false;
  }
}

static size_t appendMachineToken(char* buffer, size_t buffer_size, size_t offset,
                                 const char* format, ...)
{
  if (buffer == NULL || buffer_size == 0 || offset >= buffer_size) {
    return offset;
  }

  va_list args;
  va_start(args, format);
  int written = vsnprintf(buffer + offset, buffer_size - offset, format, args);
  va_end(args);

  if (written < 0) {
    buffer[offset] = '\0';
    return offset;
  }

  size_t next_offset = offset + (size_t) written;
  if (next_offset >= buffer_size) {
    return buffer_size - 1;
  }

  return next_offset;
}

static const char* messageRouteString(MessageType_t type)
{
  switch (type) {
    case MSG_RECEIVED_TRANSDUCER:
    case MSG_TRANSMIT_TRANSDUCER:
      return "transducer";
    case MSG_RECEIVED_FEEDBACK:
    case MSG_TRANSMIT_FEEDBACK:
      return "feedback";
    default:
      return "unknown";
  }
}

static const char* protocolString(MessagingProtocol_t protocol)
{
  switch (protocol) {
    case PROTOCOL_CUSTOM:
      return "custom";
    case PROTOCOL_JANUS:
      return "janus";
    default:
      return "unknown";
  }
}

static const char* customTypeString(CustomMessageData_t data_type)
{
  switch (data_type) {
    case INTEGER:
      return "integer";
    case STRING:
      return "string";
    case FLOAT:
      return "float";
    case BITS:
      return "bits";
    case RANGING_REQUEST:
      return "ranging_request";
    case RANGING_RESPONSE:
      return "ranging_response";
    case EVAL:
      return "eval";
    case UNKNOWN:
    default:
      return "unknown";
  }
}

static const char* janusTypeString(JanusMessageData_t data_type)
{
  switch (data_type) {
    case JANUS_011_01_SMS:
      return "sms";
    case JANUS_011_02_TXT:
      return "txt";
    case JANUS_011_03_TXT_ACK:
      return "txt_ack";
    case JANUS_UNKNOWN:
    default:
      return "unknown";
  }
}

static void appendPreambleFieldToken(char* buffer, size_t buffer_size, size_t* offset,
                                     const char* key, PreambleValue_t value)
{
  if (buffer == NULL || offset == NULL || key == NULL || value.valid != true) {
    return;
  }

  *offset = appendMachineToken(buffer, buffer_size, *offset, " %s=%u", key, value.value);
}
