/*
 * comm_commands.c
 *
 *  Created on: Mar 25, 2026
 *      Author: jac4e
 *
 * Copyright (c) 2025 OpenAquatix Contributors
 * SPDX-License-Identifier: MIT
 */

/* Private includes ----------------------------------------------------------*/

#include "comm_commands.h"
#include "cfg_import_export.h"
#include "cfg_parameters.h"
#include "comm_main.h"
#include "error_log.h"
#include "mac_main.h"
#include "mac_protocol.h"
#include "mess_main.h"
#include "main.h"
#include "cmsis_os.h"

#include "base64.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private typedef -----------------------------------------------------------*/

typedef bool (*CommandHandler_t)(CommandContext_t* context, char* args);

typedef struct {
  const char* name;
  const char* description;
  const char* usage;
  CommandHandler_t handler;
} CommandEntry_t;

/* Private function prototypes -----------------------------------------------*/

// Helper functions for command parsing and execution
static char* trimWhitespace(char* text);
static const CommandEntry_t* findCommand(const char* name);
static const char* normalizeCommandName(const char* name);
static bool stringsEqualIgnoreCase(const char* lhs, const char* rhs);
static bool parseOnOffArgument(char* args, bool* enabled);
static bool parseArguments(char* args, char* argv[], int max_args, int* argc);
static bool parseKeyValueArgument(char* arg, char** key, char** value);
static bool parseBooleanValue(const char* text, bool* enabled);
static bool parseUint32Value(const char* text, uint32_t* value);
static bool parseUint16Value(const char* text, uint16_t* value);
static bool parseTelemetryGroupValue(const char* text, TelemetryGroup_t* group);
static bool parseProtocolValue(const char* text, MessagingProtocol_t* protocol);
static bool parseCustomTypeValue(const char* text, CustomMessageData_t* data_type);
static bool parseJanusTypeValue(const char* text, JanusMessageData_t* data_type);
static const char* protocolToString(MessagingProtocol_t protocol);
static const char* macModeToString(bool host_mode_enabled);
static void sanitizeMachineToken(const char* input, char* output, size_t output_size);
static bool queueRegularMessage(const Message_t* msg);
static bool transmitMessage(const char* data, bool use_feedback);
static bool handleImmediateTransmitCommand(CommandContext_t* context, char* args,
                                           const CommandEntry_t* command,
                                           const char* command_name);
static void initializeMessage(Message_t* msg);
static bool setPreambleOverride(Message_t* msg, const char* key, const char* value);

// String conversion helpers for human-readable output
static void transmitCommandUsage(CommInterface_t interface, const CommandEntry_t* command);
static void transmitCommandHelp(CommInterface_t interface, const CommandEntry_t* command);

// Command handlers
static bool handleHelpCommand(CommandContext_t* context, char* args);
static bool handleTagCommand(CommandContext_t* context, char* args);
static bool handlePromptCommand(CommandContext_t* context, char* args);
static bool handlePrintCommand(CommandContext_t* context, char* args);
static bool handleModeCommand(CommandContext_t* context, char* args);
static bool handleTimeCommand(CommandContext_t* context, char* args);
static bool handleTransmitCommand(CommandContext_t* context, char* args);
static bool handleRangeCommand(CommandContext_t* context, char* args);
static bool handleTransmitAtCommand(CommandContext_t* context, char* args);
static bool handleCancelTxCommand(CommandContext_t* context, char* args);
static bool handleRxSubscriptionCommand(CommandContext_t* context, char* args);
static bool handleSenseSubscriptionCommand(CommandContext_t* context, char* args);
static bool handleStatusCommand(CommandContext_t* context, char* args);
static bool handleTelemetryCommand(CommandContext_t* context, char* args);
static bool handleTelemetrySubscriptionCommand(CommandContext_t* context, char* args);
static bool handleErrorLogCommand(CommandContext_t* context, char* args);
static bool handleImportConfigCommand(CommandContext_t* context, char* args);
static bool handleConfigCommand(CommandContext_t* context, char* args);

/* Private variables ---------------------------------------------------------*/

extern osMessageQueueId_t regular_tx_queue;

#define DEFAULT_TELEMETRY_PERIOD_MS    1000U
#define MIN_TELEMETRY_PERIOD_MS        100U
#define MAX_TELEMETRY_PERIOD_MS        60000U

static const CommandEntry_t commands[] = {
  {
    "help",
    "Display the available commands or detailed help for one command",
    COMM_COMMAND_DELIMITER_STR "help [command]",
    handleHelpCommand
  },
  {
    "tag",
    "Enable or disable tagged COMM HMI output for the current session",
    COMM_COMMAND_DELIMITER_STR "tag on|off",
    handleTagCommand
  },
  {
    "prompt",
    "Enable or disable HMI prompts for the current session",
    COMM_COMMAND_DELIMITER_STR "prompt on|off",
    handlePromptCommand
  },
  {
    "print",
    "Enable or disable printing of received messages",
    COMM_COMMAND_DELIMITER_STR "print on|off",
    handlePrintCommand
  },
  {
    "mode",
    "Enable or disable host-controlled MAC mode",
    COMM_COMMAND_DELIMITER_STR "mode hostmac on|off",
    handleModeCommand
  },
  {
    "time",
    "Return the current device time bases used for host scheduling",
    COMM_COMMAND_DELIMITER_STR "time",
    handleTimeCommand
  },
  {
    "tx",
    "Transmit a base64 payload immediately through the selected route",
    COMM_COMMAND_DELIMITER_STR "tx route=<transducer|feedback> payload=<base64>",
    handleTransmitCommand
  },
  {
    "range",
    "Queue an immediate ranging request through the selected route",
    COMM_COMMAND_DELIMITER_STR "range [route=transducer|feedback]",
    handleRangeCommand
  },
  {
    "txat",
    "Schedule a host-controlled transmission at a future device CYCCNT",
    COMM_COMMAND_DELIMITER_STR "txat route=<transducer|feedback> protocol=<custom|janus> payload=<base64> tx_cyccnt=<u32> [type=<custom_type>] [janus_type=sms] [modem_id=<u16>] [is_mobile=<0|1>] [reservation_time_10ms=<u16>] [schedule_flag=<u16>] [destination_id=<u16>] [tx_rx_capable=<0|1>] [can_forward=<0|1>] [coding=<u16>] [encryption=<u16>]",
    handleTransmitAtCommand
  },
  {
    "cancel_tx",
    "Cancel a pending host-scheduled transmission",
    COMM_COMMAND_DELIMITER_STR "cancel_tx <request_id>",
    handleCancelTxCommand
  },
  {
    "rxsub",
    "Enable or disable machine-readable RX event streaming",
    COMM_COMMAND_DELIMITER_STR "rxsub on|off",
    handleRxSubscriptionCommand
  },
  {
    "sense",
    "Enable or disable machine-readable channel sensing event streaming",
    COMM_COMMAND_DELIMITER_STR "sense on|off",
    handleSenseSubscriptionCommand
  },
  {
    "status",
    "Report host MAC mode, subscriptions, queue depths, and pending scheduled TX state",
    COMM_COMMAND_DELIMITER_STR "status",
    handleStatusCommand
  },
  {
    "telemetry",
    "Return a machine-readable telemetry snapshot",
    COMM_COMMAND_DELIMITER_STR "telemetry [all|temp|power|electrical|env]",
    handleTelemetryCommand
  },
  {
    "telemetrysub",
    "Enable or disable periodic machine-readable telemetry streaming",
    COMM_COMMAND_DELIMITER_STR "telemetrysub on|off [all|temp|power|electrical|env] [period_ms=<u32>]",
    handleTelemetrySubscriptionCommand
  },
  {
    "errorlog",
    "Dump the retained error log in machine-readable form",
    COMM_COMMAND_DELIMITER_STR "errorlog",
    handleErrorLogCommand
  },
  {
    "importcfg",
    "Import configuration data using the same START...END blob accepted by the menu importer",
    COMM_COMMAND_DELIMITER_STR "importcfg <START,...,END>",
    handleImportConfigCommand
  },
  {
    "config",
    "Get or set configuration parameters",
    COMM_COMMAND_DELIMITER_STR "config [get|set <parameter_name> [value]]",
    handleConfigCommand
  }
};

/* Exported function definitions ---------------------------------------------*/

bool COMM_Commands_Process(const char* input, CommandContext_t* context)
{
  char command_buffer[MAX_COMM_IN_BUFFER_SIZE];

  if (input == NULL || context == NULL) {
    return false;
  }

  strncpy(command_buffer, input, sizeof(command_buffer) - 1);
  command_buffer[sizeof(command_buffer) - 1] = '\0';

  char* cursor = trimWhitespace(command_buffer);
  if (*cursor != COMM_COMMAND_DELIMITER) {
    return false;
  }

  cursor++;
  cursor = trimWhitespace(cursor);

  context->redraw_menu = false;

  if (*cursor == '\0') {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR, "Missing command name", context->interface);
    return true;
  }

  char* args = cursor;
  while (*args != '\0' && isspace((unsigned char) *args) == 0) {
    args++;
  }

  if (*args != '\0') {
    *args = '\0';
    args = trimWhitespace(args + 1);
  }

  const CommandEntry_t* command = findCommand(cursor);
  if (command == NULL) {
    COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
        "Unknown command: %c%s", COMM_COMMAND_DELIMITER, cursor);
    return true;
  }

  if (command->handler(context, args) == false) {
    transmitCommandUsage(context->interface, command);
  }

  return true;
}

/* Private function definitions ----------------------------------------------*/

static char* trimWhitespace(char* text)
{
  if (text == NULL) {
    return NULL;
  }

  while (*text != '\0' && isspace((unsigned char) *text) != 0) {
    text++;
  }

  size_t length = strlen(text);
  while (length > 0 && isspace((unsigned char) text[length - 1]) != 0) {
    text[length - 1] = '\0';
    length--;
  }

  return text;
}

static const CommandEntry_t* findCommand(const char* name)
{
  const char* normalized_name = normalizeCommandName(name);
  if (normalized_name == NULL || *normalized_name == '\0') {
    return NULL;
  }

  for (uint32_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
    if (stringsEqualIgnoreCase(commands[i].name, normalized_name) == true) {
      return &commands[i];
    }
  }

  return NULL;
}

static const char* normalizeCommandName(const char* name)
{
  if (name == NULL) {
    return NULL;
  }

  while (*name != '\0' && isspace((unsigned char) *name) != 0) {
    name++;
  }

  if (*name == COMM_COMMAND_DELIMITER) {
    name++;
  }

  while (*name != '\0' && isspace((unsigned char) *name) != 0) {
    name++;
  }

  return name;
}

static bool stringsEqualIgnoreCase(const char* lhs, const char* rhs)
{
  if (lhs == NULL || rhs == NULL) {
    return false;
  }

  while (*lhs != '\0' && *rhs != '\0') {
    if (tolower((unsigned char) *lhs) != tolower((unsigned char) *rhs)) {
      return false;
    }
    lhs++;
    rhs++;
  }

  return *lhs == '\0' && *rhs == '\0';
}

static bool parseOnOffArgument(char* args, bool* enabled)
{
  char* argv[1];
  int argc = 0;

  if (enabled == NULL) {
    return false;
  }

  if (parseArguments(args, argv, 1, &argc) == false || argc != 1) {
    return false;
  }

  return parseBooleanValue(argv[0], enabled);
}

static bool parseArguments(char* args, char* argv[], int max_args, int* argc)
{
  int parsed_argc = 0;
  char* read_cursor;
  char* write_cursor;

  if (argc == NULL || max_args < 0 || (max_args > 0 && argv == NULL)) {
    return false;
  }

  *argc = 0;

  if (args == NULL) {
    return true;
  }

  read_cursor = trimWhitespace(args);
  if (read_cursor == NULL || *read_cursor == '\0') {
    return true;
  }

  write_cursor = read_cursor;

  while (*read_cursor != '\0') {
    char quote = '\0';

    while (*read_cursor != '\0' && isspace((unsigned char) *read_cursor) != 0) {
      read_cursor++;
    }

    if (*read_cursor == '\0') {
      break;
    }

    if (parsed_argc >= max_args) {
      return false;
    }

    argv[parsed_argc++] = write_cursor;

    while (*read_cursor != '\0') {
      char current = *read_cursor++;

      if (current == '\\') {
        if (*read_cursor != '\0') {
          *write_cursor++ = *read_cursor++;
        }
        else {
          *write_cursor++ = current;
        }
        continue;
      }

      if (quote != '\0') {
        if (current == quote) {
          quote = '\0';
        }
        else {
          *write_cursor++ = current;
        }
        continue;
      }

      if (current == '"' || current == '\'') {
        quote = current;
        continue;
      }

      if (isspace((unsigned char) current) != 0) {
        break;
      }

      *write_cursor++ = current;
    }

    if (quote != '\0') {
      return false;
    }

    *write_cursor++ = '\0';
  }

  *argc = parsed_argc;
  return true;
}

static bool parseKeyValueArgument(char* arg, char** key, char** value)
{
  if (arg == NULL || key == NULL || value == NULL) {
    return false;
  }

  char* equals = strchr(arg, '=');
  if (equals == NULL || equals == arg || *(equals + 1) == '\0') {
    return false;
  }

  *equals = '\0';
  *key = arg;
  *value = equals + 1;
  return true;
}

static bool parseBooleanValue(const char* text, bool* enabled)
{
  if (text == NULL || enabled == NULL) {
    return false;
  }

  if (stringsEqualIgnoreCase(text, "on") == true ||
      stringsEqualIgnoreCase(text, "true") == true ||
      stringsEqualIgnoreCase(text, "yes") == true ||
      strcmp(text, "1") == 0) {
    *enabled = true;
    return true;
  }

  if (stringsEqualIgnoreCase(text, "off") == true ||
      stringsEqualIgnoreCase(text, "false") == true ||
      stringsEqualIgnoreCase(text, "no") == true ||
      strcmp(text, "0") == 0) {
    *enabled = false;
    return true;
  }

  return false;
}

static bool parseUint32Value(const char* text, uint32_t* value)
{
  if (text == NULL || value == NULL || *text == '\0') {
    return false;
  }

  char* end_ptr = NULL;
  unsigned long parsed_value = strtoul(text, &end_ptr, 0);
  if (*end_ptr != '\0') {
    return false;
  }

  *value = (uint32_t) parsed_value;
  return true;
}

static bool parseUint16Value(const char* text, uint16_t* value)
{
  uint32_t parsed_value;
  if (parseUint32Value(text, &parsed_value) == false || parsed_value > UINT16_MAX) {
    return false;
  }

  *value = (uint16_t) parsed_value;
  return true;
}

static bool parseTelemetryGroupValue(const char* text, TelemetryGroup_t* group)
{
  if (text == NULL || group == NULL) {
    return false;
  }

  if (stringsEqualIgnoreCase(text, "all") == true) {
    *group = TELEMETRY_GROUP_ALL;
    return true;
  }
  if (stringsEqualIgnoreCase(text, "temp") == true) {
    *group = TELEMETRY_GROUP_TEMP;
    return true;
  }
  if (stringsEqualIgnoreCase(text, "power") == true) {
    *group = TELEMETRY_GROUP_POWER;
    return true;
  }
  if (stringsEqualIgnoreCase(text, "electrical") == true) {
    *group = TELEMETRY_GROUP_ELECTRICAL;
    return true;
  }
  if (stringsEqualIgnoreCase(text, "env") == true) {
    *group = TELEMETRY_GROUP_ENV;
    return true;
  }

  return false;
}

static bool parseProtocolValue(const char* text, MessagingProtocol_t* protocol)
{
  if (text == NULL || protocol == NULL) {
    return false;
  }

  if (stringsEqualIgnoreCase(text, "custom") == true) {
    *protocol = PROTOCOL_CUSTOM;
    return true;
  }

  if (stringsEqualIgnoreCase(text, "janus") == true) {
    *protocol = PROTOCOL_JANUS;
    return true;
  }

  return false;
}

static bool parseCustomTypeValue(const char* text, CustomMessageData_t* data_type)
{
  if (text == NULL || data_type == NULL) {
    return false;
  }

  if (stringsEqualIgnoreCase(text, "integer") == true) {
    *data_type = INTEGER;
    return true;
  }
  if (stringsEqualIgnoreCase(text, "string") == true) {
    *data_type = STRING;
    return true;
  }
  if (stringsEqualIgnoreCase(text, "float") == true) {
    *data_type = FLOAT;
    return true;
  }
  if (stringsEqualIgnoreCase(text, "bits") == true) {
    *data_type = BITS;
    return true;
  }
  if (stringsEqualIgnoreCase(text, "ranging_request") == true) {
    *data_type = RANGING_REQUEST;
    return true;
  }
  if (stringsEqualIgnoreCase(text, "ranging_response") == true) {
    *data_type = RANGING_RESPONSE;
    return true;
  }
  if (stringsEqualIgnoreCase(text, "eval") == true) {
    *data_type = EVAL;
    return true;
  }

  return false;
}

static bool parseJanusTypeValue(const char* text, JanusMessageData_t* data_type)
{
  if (text == NULL || data_type == NULL) {
    return false;
  }

  if (stringsEqualIgnoreCase(text, "sms") == true) {
    *data_type = JANUS_011_01_SMS;
    return true;
  }

  return false;
}

static const char* protocolToString(MessagingProtocol_t protocol)
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

static const char* macModeToString(bool host_mode_enabled)
{
  return host_mode_enabled ? "hostmac" : "local";
}

static void sanitizeMachineToken(const char* input, char* output, size_t output_size)
{
  size_t out_index = 0;
  bool previous_was_underscore = false;

  if (output == NULL || output_size == 0) {
    return;
  }

  if (input == NULL) {
    output[0] = '\0';
    return;
  }

  while (*input != '\0' && out_index + 1 < output_size) {
    unsigned char current = (unsigned char) *input++;
    if (isalnum(current) != 0) {
      output[out_index++] = (char) tolower(current);
      previous_was_underscore = false;
      continue;
    }

    if (previous_was_underscore == false) {
      output[out_index++] = '_';
      previous_was_underscore = true;
    }
  }

  if (out_index > 0 && output[out_index - 1] == '_') {
    out_index--;
  }

  output[out_index] = '\0';
}

static bool queueRegularMessage(const Message_t* msg)
{
  if (regular_tx_queue == NULL || msg == NULL) {
    return false;
  }

  return osMessageQueuePut(regular_tx_queue, msg, 0, 0) == osOK;
}

static bool transmitMessage(const char* data, bool use_feedback)
{
  if (data == NULL) {
    return false;
  }

  size_t decoded_length = 0;
  unsigned char* decoded_data = base64_decode((const unsigned char*) data, strlen(data), &decoded_length);
  if (decoded_data == NULL || decoded_length > PACKET_DATA_MAX_LENGTH_BYTES) {
    free(decoded_data);
    return false;
  }

  Message_t msg;
  initializeMessage(&msg);
  msg.protocol = PROTOCOL_CUSTOM;
  msg.type = use_feedback ? MSG_TRANSMIT_FEEDBACK : MSG_TRANSMIT_TRANSDUCER;
  msg.timestamp = osKernelGetTickCount();
  msg.data_type = BITS;
  msg.preamble.message_type.value = BITS;
  msg.preamble.message_type.valid = true;
  msg.length_bits = decoded_length * 8U;
  memcpy(msg.data, decoded_data, decoded_length);

  free(decoded_data);
  return queueRegularMessage(&msg);
}

static bool handleImmediateTransmitCommand(CommandContext_t* context, char* args,
                                           const CommandEntry_t* command,
                                           const char* command_name)
{
  char* argv[8];
  int argc = 0;
  bool use_feedback = false;
  bool have_route = false;
  bool have_payload = false;
  const char* payload = NULL;

  if (context == NULL || command == NULL || command_name == NULL) {
    return false;
  }

  if (parseArguments(args, argv, 8, &argc) == false || argc == 0) {
    return false;
  }

  for (int i = 0; i < argc; i++) {
    char* key = NULL;
    char* value = NULL;
    if (parseKeyValueArgument(argv[i], &key, &value) == false) {
      COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
          "Invalid %s argument: %s", command_name, argv[i]);
      return true;
    }

    if (stringsEqualIgnoreCase(key, "route") == true) {
      if (stringsEqualIgnoreCase(value, "transducer") == true) {
        use_feedback = false;
        have_route = true;
      }
      else if (stringsEqualIgnoreCase(value, "feedback") == true) {
        use_feedback = true;
        have_route = true;
      }
      else {
        COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
            "Invalid route: %s", value);
        return true;
      }
      continue;
    }

    if (stringsEqualIgnoreCase(key, "payload") == true ||
        stringsEqualIgnoreCase(key, "data") == true ||
        stringsEqualIgnoreCase(key, "b64") == true) {
      payload = value;
      have_payload = true;
      continue;
    }

    COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
        "Unknown %s option: %s", command_name, key);
    return true;
  }

  if (have_route == false || have_payload == false || payload == NULL || *payload == '\0') {
    transmitCommandUsage(context->interface, command);
    return true;
  }

  if (transmitMessage(payload, use_feedback) == false) {
    COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
        "Failed to queue %s transmission",
        use_feedback ? "feedback" : "transducer");
    return true;
  }

  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %c%s route=%s",
      COMM_COMMAND_DELIMITER,
      command_name,
      use_feedback ? "feedback" : "transducer");
  return true;
}

static void initializeMessage(Message_t* msg)
{
  if (msg == NULL) {
    return;
  }

  memset(msg, 0, sizeof(*msg));
  msg->protocol = PROTOCOL_CUSTOM;
  msg->delay = false;
}

static bool setPreambleOverride(Message_t* msg, const char* key, const char* value)
{
  uint16_t parsed_u16 = 0;
  bool parsed_bool = false;

  if (msg == NULL || key == NULL || value == NULL) {
    return false;
  }

  if (stringsEqualIgnoreCase(key, "modem_id") == true) {
    if (parseUint16Value(value, &parsed_u16) == false) return false;
    msg->preamble.modem_id.value = parsed_u16;
    msg->preamble.modem_id.valid = true;
    return true;
  }
  if (stringsEqualIgnoreCase(key, "is_mobile") == true) {
    if (parseBooleanValue(value, &parsed_bool) == false) return false;
    msg->preamble.is_mobile.value = parsed_bool ? 1U : 0U;
    msg->preamble.is_mobile.valid = true;
    return true;
  }
  if (stringsEqualIgnoreCase(key, "reservation_time_10ms") == true) {
    if (parseUint16Value(value, &parsed_u16) == false) return false;
    msg->preamble.reservation_time_10ms.value = parsed_u16;
    msg->preamble.reservation_time_10ms.valid = true;
    return true;
  }
  if (stringsEqualIgnoreCase(key, "schedule_flag") == true) {
    if (parseUint16Value(value, &parsed_u16) == false) return false;
    msg->preamble.schedule_flag.value = parsed_u16;
    msg->preamble.schedule_flag.valid = true;
    return true;
  }
  if (stringsEqualIgnoreCase(key, "destination_id") == true) {
    if (parseUint16Value(value, &parsed_u16) == false) return false;
    msg->preamble.destination_id.value = parsed_u16;
    msg->preamble.destination_id.valid = true;
    return true;
  }
  if (stringsEqualIgnoreCase(key, "tx_rx_capable") == true) {
    if (parseBooleanValue(value, &parsed_bool) == false) return false;
    msg->preamble.tx_rx_capable.value = parsed_bool ? 1U : 0U;
    msg->preamble.tx_rx_capable.valid = true;
    return true;
  }
  if (stringsEqualIgnoreCase(key, "can_forward") == true) {
    if (parseBooleanValue(value, &parsed_bool) == false) return false;
    msg->preamble.can_forward.value = parsed_bool ? 1U : 0U;
    msg->preamble.can_forward.valid = true;
    return true;
  }
  if (stringsEqualIgnoreCase(key, "coding") == true) {
    if (parseUint16Value(value, &parsed_u16) == false) return false;
    msg->preamble.coding.value = parsed_u16;
    msg->preamble.coding.valid = true;
    return true;
  }
  if (stringsEqualIgnoreCase(key, "encryption") == true) {
    if (parseUint16Value(value, &parsed_u16) == false) return false;
    msg->preamble.encryption.value = parsed_u16;
    msg->preamble.encryption.valid = true;
    return true;
  }

  return false;
}

static void transmitCommandUsage(CommInterface_t interface, const CommandEntry_t* command)
{
  COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, interface, "Usage: %s", command->usage);
}

static void transmitCommandHelp(CommInterface_t interface, const CommandEntry_t* command)
{
  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, interface,
      "Command: %c%s", COMM_COMMAND_DELIMITER, command->name);
  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, interface,
      "Description: %s", command->description);
  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, interface,
      "Usage: %s", command->usage);
}

static bool handleHelpCommand(CommandContext_t* context, char* args)
{
  char* argv[1];
  int argc = 0;

  if (context == NULL) {
    return false;
  }

  if (parseArguments(args, argv, 1, &argc) == false) {
    return false;
  }

  if (argc == 0) {
    COMM_TransmitHmiMachineLine(HMI_TAG_STATUS, "Available commands:", context->interface);

    for (uint32_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
      COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
          "%c%s - %s",
          COMM_COMMAND_DELIMITER,
          commands[i].name,
          commands[i].description);
    }

    return true;
  }

  const CommandEntry_t* command = findCommand(argv[0]);
  if (command == NULL) {
    COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
        "Unknown command: %s%s",
        COMM_COMMAND_DELIMITER_STR,
        normalizeCommandName(argv[0]));
    return true;
  }

  transmitCommandHelp(context->interface, command);
  return true;
}

static bool handleTagCommand(CommandContext_t* context, char* args)
{
  bool enabled;

  if (parseOnOffArgument(args, &enabled) == false) {
    return false;
  }

  COMM_SetTagMode(enabled);
  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %ctag %s", COMM_COMMAND_DELIMITER, enabled ? "on" : "off");
  return true;
}

static bool handlePromptCommand(CommandContext_t* context, char* args)
{
  bool enabled;

  if (parseOnOffArgument(args, &enabled) == false) {
    return false;
  }

  COMM_SetPromptEnabled(enabled);
  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %cprompt %s", COMM_COMMAND_DELIMITER, enabled ? "on" : "off");
  return true;
}

static bool handlePrintCommand(CommandContext_t* context, char* args)
{
  bool enabled;

  if (parseOnOffArgument(args, &enabled) == false) {
    return false;
  }

  uint8_t print_enabled = enabled ? 1U : 0U;
  if (Param_SetUint8(PARAM_PRINT_ENABLED, &print_enabled) == PARAM_SET_SUCCESS) {
    COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
        "OK %cprint %s", COMM_COMMAND_DELIMITER, enabled ? "on" : "off");
  }
  else {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
        "Failed to set printing received messages", context->interface);
  }

  return true;
}

static bool handleModeCommand(CommandContext_t* context, char* args)
{
  char* argv[2];
  int argc = 0;

  if (parseArguments(args, argv, 2, &argc) == false || argc != 2) {
    return false;
  }

  if (stringsEqualIgnoreCase(argv[0], "hostmac") == false) {
    return false;
  }

  bool enabled;
  if (parseBooleanValue(argv[1], &enabled) == false) {
    return false;
  }

  uint8_t protocol = enabled ? MAC_PROTOCOL_HOST_CONTROL : MAC_PROTOCOL_NONE;
  if (Param_SetUint8(PARAM_MAC, &protocol) != PARAM_SET_SUCCESS) {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
        "Failed to update MAC mode", context->interface);
    return true;
  }

  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %cmode hostmac %s", COMM_COMMAND_DELIMITER, enabled ? "on" : "off");
  return true;
}

static bool handleTimeCommand(CommandContext_t* context, char* args)
{
  char* argv[1];
  int argc = 0;

  if (parseArguments(args, argv, 1, &argc) == false || argc != 0) {
    return false;
  }

  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %ctime tick_ms=%llu cyccnt=%lu",
      COMM_COMMAND_DELIMITER,
      (unsigned long long) HAL_AbsoluteTimestamp(),
      (unsigned long) DWT->CYCCNT);
  return true;
}

static bool handleTransmitCommand(CommandContext_t* context, char* args)
{
  return handleImmediateTransmitCommand(context, args, &commands[6], "tx");
}

static bool handleRangeCommand(CommandContext_t* context, char* args)
{
  char* argv[1];
  int argc = 0;
  bool use_feedback = false;

  if (parseArguments(args, argv, 1, &argc) == false || argc > 1) {
    return false;
  }

  if (argc == 1) {
    char* key = NULL;
    char* value = NULL;

    if (parseKeyValueArgument(argv[0], &key, &value) == false ||
        stringsEqualIgnoreCase(key, "route") == false) {
      return false;
    }

    if (stringsEqualIgnoreCase(value, "transducer") == true) {
      use_feedback = false;
    }
    else if (stringsEqualIgnoreCase(value, "feedback") == true) {
      use_feedback = true;
    }
    else {
      return false;
    }
  }

  if (print_event_handle == NULL) {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
        "Failed to queue ranging request", context->interface);
    return true;
  }

  uint32_t flags = osEventFlagsSet(print_event_handle,
      use_feedback ? MESS_REQUEST_RANGE_FEEDBACK : MESS_REQUEST_RANGE_TRANSDUCER);
  if (flags & osFlagsError) {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
        "Failed to queue ranging request", context->interface);
    return true;
  }

  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %crange route=%s", COMM_COMMAND_DELIMITER,
      use_feedback ? "feedback" : "transducer");
  return true;
}

static bool handleTransmitAtCommand(CommandContext_t* context, char* args)
{
  char* argv[20];
  int argc = 0;

  if (parseArguments(args, argv, 20, &argc) == false || argc < 4) {
    return false;
  }

  if (MAC_IsHostModeEnabled() == false) {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
        "Host MAC mode must be enabled before using :txat", context->interface);
    return true;
  }

  Message_t msg;
  initializeMessage(&msg);
  msg.timestamp = osKernelGetTickCount();
  msg.delay = true;
  msg.janus_data_type = JANUS_011_01_SMS;

  bool have_route = false;
  bool have_protocol = false;
  bool have_payload = false;
  bool have_tx_cyccnt = false;
  bool have_custom_type = false;

  for (int i = 0; i < argc; i++) {
    char* key = NULL;
    char* value = NULL;
    if (parseKeyValueArgument(argv[i], &key, &value) == false) {
      COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
          "Invalid txat argument: %s", argv[i]);
      return true;
    }

    if (stringsEqualIgnoreCase(key, "route") == true) {
      if (stringsEqualIgnoreCase(value, "transducer") == true) {
        msg.type = MSG_TRANSMIT_TRANSDUCER;
        have_route = true;
      }
      else if (stringsEqualIgnoreCase(value, "feedback") == true) {
        msg.type = MSG_TRANSMIT_FEEDBACK;
        have_route = true;
      }
      else {
        COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
            "Invalid route: %s", value);
        return true;
      }
      continue;
    }

    if (stringsEqualIgnoreCase(key, "protocol") == true) {
      if (parseProtocolValue(value, &msg.protocol) == false) {
        COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
            "Invalid protocol: %s", value);
        return true;
      }
      have_protocol = true;
      continue;
    }

    if (stringsEqualIgnoreCase(key, "payload") == true ||
        stringsEqualIgnoreCase(key, "data") == true ||
        stringsEqualIgnoreCase(key, "b64") == true) {
      size_t decoded_length = 0;
      unsigned char* decoded_data = base64_decode((const unsigned char*) value, strlen(value), &decoded_length);
      if (decoded_data == NULL || decoded_length > PACKET_DATA_MAX_LENGTH_BYTES) {
        free(decoded_data);
        COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
            "Failed to decode payload or payload exceeds maximum size", context->interface);
        return true;
      }
      memcpy(msg.data, decoded_data, decoded_length);
      msg.length_bits = decoded_length * 8U;
      free(decoded_data);
      have_payload = true;
      continue;
    }

    if (stringsEqualIgnoreCase(key, "tx_cyccnt") == true) {
      if (parseUint32Value(value, &msg.delay_cyccnt) == false) {
        COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
            "Invalid tx_cyccnt: %s", value);
        return true;
      }
      have_tx_cyccnt = true;
      continue;
    }

    if (stringsEqualIgnoreCase(key, "type") == true ||
        stringsEqualIgnoreCase(key, "custom_type") == true) {
      if (parseCustomTypeValue(value, &msg.data_type) == false) {
        COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
            "Invalid custom type: %s", value);
        return true;
      }
      msg.preamble.message_type.value = msg.data_type;
      msg.preamble.message_type.valid = true;
      have_custom_type = true;
      continue;
    }

    if (stringsEqualIgnoreCase(key, "janus_type") == true) {
      if (parseJanusTypeValue(value, &msg.janus_data_type) == false) {
        COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
            "Invalid janus type: %s", value);
        return true;
      }
      continue;
    }

    if (setPreambleOverride(&msg, key, value) == false) {
      COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
          "Unknown or invalid txat option: %s", key);
      return true;
    }
  }

  if (have_route == false || have_protocol == false ||
      have_payload == false || have_tx_cyccnt == false) {
    return false;
  }

  if (msg.protocol == PROTOCOL_CUSTOM && have_custom_type == false) {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
        "Custom protocol transmissions require type=<custom_type>", context->interface);
    return true;
  }

  uint32_t request_id = 0;
  if (MAC_ScheduleHostMessage(&msg, &request_id) == false) {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
        "Failed to schedule transmission; tx_cyccnt may be too close, in the past, beyond the safe wrap window, or another scheduled TX is already pending",
        context->interface);
    return true;
  }

  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %ctxat request_id=%lu protocol=%s tx_cyccnt=%lu",
      COMM_COMMAND_DELIMITER, (unsigned long) request_id,
      protocolToString(msg.protocol), (unsigned long) msg.delay_cyccnt);
  return true;
}

static bool handleCancelTxCommand(CommandContext_t* context, char* args)
{
  char* argv[1];
  int argc = 0;
  uint32_t request_id = 0;

  if (parseArguments(args, argv, 1, &argc) == false || argc != 1) {
    return false;
  }

  if (parseUint32Value(argv[0], &request_id) == false || request_id == 0) {
    COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
        "Invalid request id: %s", argv[0]);
    return true;
  }

  if (MAC_CancelHostMessage(request_id) == false) {
    COMM_TransmitHmiMachineLinef(HMI_TAG_ERROR, context->interface,
        "No pending scheduled transmission with id=%lu", (unsigned long) request_id);
    return true;
  }

  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %ccancel_tx request_id=%lu", COMM_COMMAND_DELIMITER, (unsigned long) request_id);
  return true;
}

static bool handleRxSubscriptionCommand(CommandContext_t* context, char* args)
{
  bool enabled;

  if (parseOnOffArgument(args, &enabled) == false) {
    return false;
  }

  COMM_SetHostRxSubscription(enabled, context->interface);
  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %crxsub %s", COMM_COMMAND_DELIMITER, enabled ? "on" : "off");
  return true;
}

static bool handleSenseSubscriptionCommand(CommandContext_t* context, char* args)
{
  bool enabled;

  if (parseOnOffArgument(args, &enabled) == false) {
    return false;
  }

  COMM_SetHostSenseSubscription(enabled, context->interface);
  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %csense %s", COMM_COMMAND_DELIMITER, enabled ? "on" : "off");
  return true;
}

static bool handleTelemetryCommand(CommandContext_t* context, char* args)
{
  char* argv[1];
  int argc = 0;
  TelemetryGroup_t group = TELEMETRY_GROUP_ALL;

  if (parseArguments(args, argv, 1, &argc) == false || argc > 1) {
    return false;
  }

  if (argc == 1 && parseTelemetryGroupValue(argv[0], &group) == false) {
    return false;
  }

  if (COMM_TransmitTelemetrySnapshot(context->interface, group) == false) {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
        "Failed to build telemetry snapshot", context->interface);
  }

  return true;
}

static bool handleTelemetrySubscriptionCommand(CommandContext_t* context, char* args)
{
  char* argv[3];
  int argc = 0;
  bool enabled = false;
  TelemetryGroup_t group = TELEMETRY_GROUP_ALL;
  uint32_t period_ms = DEFAULT_TELEMETRY_PERIOD_MS;
  bool group_seen = false;
  bool period_seen = false;

  if (parseArguments(args, argv, 3, &argc) == false || argc < 1 || argc > 3) {
    return false;
  }

  if (parseBooleanValue(argv[0], &enabled) == false) {
    return false;
  }

  if (enabled == false) {
    if (argc != 1) {
      return false;
    }

    if (COMM_SetHostTelemetrySubscription(false, TELEMETRY_GROUP_NONE, 0,
                                          context->interface) == false) {
      COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
          "Failed to disable telemetry stream", context->interface);
      return true;
    }

    COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
        "OK %ctelemetrysub off", COMM_COMMAND_DELIMITER);
    return true;
  }

  for (int i = 1; i < argc; i++) {
    char* key = NULL;
    char* value = NULL;

    if (strchr(argv[i], '=') != NULL) {
      if (parseKeyValueArgument(argv[i], &key, &value) == false ||
          stringsEqualIgnoreCase(key, "period_ms") == false ||
          period_seen == true ||
          parseUint32Value(value, &period_ms) == false ||
          period_ms < MIN_TELEMETRY_PERIOD_MS ||
          period_ms > MAX_TELEMETRY_PERIOD_MS) {
        return false;
      }
      period_seen = true;
      continue;
    }

    if (group_seen == true || parseTelemetryGroupValue(argv[i], &group) == false) {
      return false;
    }
    group_seen = true;
  }

  if (COMM_SetHostTelemetrySubscription(true, group, period_ms, context->interface) == false) {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
        "Failed to configure telemetry stream", context->interface);
    return true;
  }

  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %ctelemetrysub on group=%s period_ms=%lu", COMM_COMMAND_DELIMITER,
      group == TELEMETRY_GROUP_TEMP ? "temp" :
      group == TELEMETRY_GROUP_POWER ? "power" :
      group == TELEMETRY_GROUP_ELECTRICAL ? "electrical" :
      group == TELEMETRY_GROUP_ENV ? "env" : "all",
      (unsigned long) period_ms);
  return true;
}

static bool handleStatusCommand(CommandContext_t* context, char* args)
{
  char* argv[1];
  int argc = 0;

  if (parseArguments(args, argv, 1, &argc) == false || argc != 0) {
    return false;
  }

  MacHostStatus_t status;
  if (MAC_GetHostStatus(&status) == false) {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
        "Failed to retrieve MAC status", context->interface);
    return true;
  }

  TelemetrySubscriptionStatus_t telemetry_status;
  if (COMM_GetHostTelemetrySubscriptionStatus(&telemetry_status) == false) {
    COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
        "Failed to retrieve telemetry status", context->interface);
    return true;
  }

  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %cstatus mode=%s rxsub=%s sense=%s mac_regular_tx_depth=%lu mac_emergency_tx_depth=%lu mess_tx_depth=%lu pending_scheduled_tx=%s request_id=%lu tx_cyccnt=%lu telemetrysub=%s telemetry_group=%s telemetry_period_ms=%lu",
      COMM_COMMAND_DELIMITER,
      macModeToString(status.host_mode_enabled),
      COMM_IsHostRxSubscriptionEnabled() ? "on" : "off",
      COMM_IsHostSenseSubscriptionEnabled() ? "on" : "off",
      (unsigned long) status.regular_tx_depth,
      (unsigned long) status.emergency_tx_depth,
      (unsigned long) MESS_GetTxQueueDepth(),
      status.scheduled_tx_pending ? "yes" : "no",
      (unsigned long) status.scheduled_tx_id,
      (unsigned long) status.scheduled_tx_cyccnt,
      telemetry_status.enabled ? "on" : "off",
      telemetry_status.group == TELEMETRY_GROUP_TEMP ? "temp" :
      telemetry_status.group == TELEMETRY_GROUP_POWER ? "power" :
      telemetry_status.group == TELEMETRY_GROUP_ELECTRICAL ? "electrical" :
      telemetry_status.group == TELEMETRY_GROUP_ENV ? "env" :
      telemetry_status.group == TELEMETRY_GROUP_ALL ? "all" : "none",
      (unsigned long) telemetry_status.period_ms);
  return true;
}

static bool handleErrorLogCommand(CommandContext_t* context, char* args)
{
  char* argv[1];
  int argc = 0;
  ErrorEntry_t entries[MAX_ENTRIES_IN_ERROR_LOG];
  uint32_t current_reset_count = 0;
  uint64_t current_timestamp = 0;

  if (parseArguments(args, argv, 1, &argc) == false || argc != 0) {
    return false;
  }

  uint16_t count = ErrorLog_CopySnapshot(entries, MAX_ENTRIES_IN_ERROR_LOG,
                                         &current_reset_count, &current_timestamp);

  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %cerrorlog count=%u current_tick_ms=%llu current_reset_count=%lu",
      COMM_COMMAND_DELIMITER, count, (unsigned long long) current_timestamp,
      (unsigned long) current_reset_count);

  for (uint16_t i = 0; i < count; i++) {
    const char* description = Error_GetDescription((OpenAquatixErrors_t) entries[i].error_code);
    const char* severity = Error_GetSeverity((OpenAquatixErrors_t) entries[i].error_code);
    char severity_token[32];
    sanitizeMachineToken(severity, severity_token, sizeof(severity_token));

    size_t encoded_length = 0;
    const char* safe_description = (description != NULL) ? description : "";
    unsigned char* encoded_description =
        base64_encode((const unsigned char*) safe_description, strlen(safe_description),
                      &encoded_length);
    if (encoded_description == NULL) {
      COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
          "Failed to encode error log description", context->interface);
      return true;
    }

    COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
        "ENTRY %cerrorlog index=%u timestamp_ms=%llu reset_count=%lu error_code=%lu severity=%s file=%s task=%s line=%lu occurrences=%lu description_b64=%s",
        COMM_COMMAND_DELIMITER, i, (unsigned long long) entries[i].timestamp,
        (unsigned long) entries[i].reset_count, (unsigned long) entries[i].error_code,
        severity_token[0] != '\0' ? severity_token : "unknown",
        entries[i].file_name != NULL ? entries[i].file_name : "unknown",
        entries[i].task_name != NULL ? entries[i].task_name : "unknown",
        (unsigned long) entries[i].line_number, (unsigned long) entries[i].occurrences,
        (char*) encoded_description);
    free(encoded_description);
  }

  return true;
}

static bool handleImportConfigCommand(CommandContext_t* context, char* args)
{
  char* argv[1];
  int argc = 0;
  uint8_t output_buffer[MAX_COMM_OUT_BUFFER_SIZE];

  if (context == NULL) {
    return false;
  }

  if (parseArguments(args, argv, 1, &argc) == false || argc != 1) {
    return false;
  }

  if (ImportExport_ImportConfigurationText(argv[0], (uint16_t) strlen(argv[0]),
                                           output_buffer, context->interface) == false) {
    return true;
  }

  COMM_TransmitHmiMachineLinef(HMI_TAG_STATUS, context->interface,
      "OK %cimportcfg", COMM_COMMAND_DELIMITER);
  return true;
}

static bool handleConfigCommand(CommandContext_t* context, char* args)
{
  (void) args;
  COMM_TransmitHmiMachineLine(HMI_TAG_ERROR,
      "Config command not implemented yet", context->interface);
  return true;
}
