/*
 * cfg_import_export.c
 *
 *  Created on: Apr 17, 2025
 *      Author: ericv
 *
 * Copyright (c) 2025 OpenAquatix Contributors
 * SPDX-License-Identifier: MIT
 */

/* Private includes ----------------------------------------------------------*/

#include "cfg_import_export.h"
#include "cfg_parameters.h"

#include "comm_menu_system.h"
#include "comm_main.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* Private define ------------------------------------------------------------*/

#define START_SOME_SEQUENCE   "START_SOME"
#define END_SOME_SEQUENCE     "END_SOME"
#define START_ALL_SEQUENCE    "START_ALL"
#define END_ALL_SEQUENCE      "END_ALL"
// The end of a message is delimited by a newline so a comma is required when
// pasting since otherwise the pasted contents will be cut prematurely
#define DELIMITER             ","

/* Private variables ---------------------------------------------------------*/

// List of parameters exported by ExportSomeParameters. New parameters can be
// added anywhere in the list and parameters do not need to be in order
static ParamIds_t some_parameters[] = {
    PARAM_BAUD,
    PARAM_FSK_F0,
    PARAM_FSK_F1,
    PARAM_MOD_DEMOD_METHOD,
    PARAM_FC,
    PARAM_FHBFSK_FREQ_SPACING,
    PARAM_FHBFSK_DWELL_TIME,
    PARAM_FHBFSK_NUM_TONES,
    PARAM_PREAMBLE_ERROR_DETECTION,
    PARAM_CARGO_ERROR_DETECTION,
    PARAM_ECC_PREAMBLE,
    PARAM_ECC_MESSAGE,
    PARAM_USE_INTERLEAVER,
    PARAM_FHBFSK_HOPPER,
    PARAM_SYNC_METHOD
};

static const uint16_t num_some_parameters =
    sizeof(some_parameters) / sizeof(some_parameters[0]);

/* Private function prototypes -----------------------------------------------*/

static bool exportOneParameter(FunctionContext_t* context, ParamIds_t id);
static void transmitExportHeader(FunctionContext_t* context,
    const char* start_marker, const char* end_marker);
static bool parseAndSetOneParameter(FunctionContext_t* context,
    const char** curr);
static bool importCore(FunctionContext_t* context, const char* prompt,
    const char* start_marker, const char* end_marker,
    bool check_count, uint16_t expected_count);

/* Exported function definitions ---------------------------------------------*/

// Exports the parameters listed in some_parameters[]. Fails immediately if
// any parameter cannot be exported
bool ImportExport_ExportSomeParameters(FunctionContext_t* context)
{
  if (context == NULL || context->state == NULL || context->output_buffer == NULL) {
    return false;
  }

  transmitExportHeader(context, START_SOME_SEQUENCE, END_SOME_SEQUENCE);

  for (uint16_t i = 0; i < num_some_parameters; i++) {
    if (exportOneParameter(context, some_parameters[i]) == false) {
      return false;
    }
  }

  COMM_TransmitData(END_SOME_SEQUENCE "\r\n", CALC_LEN, context->comm_interface);
  COMM_TransmitHmiLinef(HMI_TAG_STATUS, context->comm_interface,
      "CFG_EXPORT_DONE scope=some count=%u",
      (unsigned int) num_some_parameters);
  context->state->state = PARAM_STATE_COMPLETE;
  return true;
}

// Exports all parameters from ID 0 to NUM_PARAM. Parameters that cannot be
// exported (unknown type, getter failure, or unhandled case) are skipped
bool ImportExport_ExportAllParameters(FunctionContext_t* context)
{
  if (context == NULL || context->state == NULL || context->output_buffer == NULL) {
    return false;
  }

  transmitExportHeader(context, START_ALL_SEQUENCE, END_ALL_SEQUENCE);

  uint16_t params_exported = 0;
  for (uint16_t id = 0; id < NUM_PARAM; id++) {
    if (exportOneParameter(context, (ParamIds_t) id) == true) {
      params_exported++;
    }
  }

  COMM_TransmitData(END_ALL_SEQUENCE "\r\n", CALC_LEN, context->comm_interface);
  COMM_TransmitHmiLinef(HMI_TAG_STATUS, context->comm_interface,
      "CFG_EXPORT_DONE scope=all count=%u",
      (unsigned int) params_exported);
  context->state->state = PARAM_STATE_COMPLETE;
  return true;
}

// Imports the parameters listed in some_parameters[]. Fails if the number
// of parsed parameters does not match the expected count
bool ImportExport_ImportSomeParameters(FunctionContext_t* context)
{
  return importCore(context,
  "Paste config blob from " START_SOME_SEQUENCE " to " END_SOME_SEQUENCE,
      START_SOME_SEQUENCE,
      END_SOME_SEQUENCE,
      true,
      num_some_parameters);
}

// Imports all parameters found between the markers. Does not require a
// specific number of entries since some IDs may have been skipped on export
bool ImportExport_ImportAllParameters(FunctionContext_t* context)
{
  return importCore(context,
  "Paste config blob from " START_ALL_SEQUENCE " to " END_ALL_SEQUENCE,
      START_ALL_SEQUENCE,
      END_ALL_SEQUENCE,
      false,
      0);
}

// Imports a full START...END configuration blob from a provided input buffer
// instead of the interactive menu state machine input path
bool ImportExport_ImportConfigurationText(const char* input, uint16_t input_len,
                                          uint8_t* output_buffer,
                                          CommInterface_t interface)
{
  if (input == NULL || output_buffer == NULL) {
    return false;
  }

  ParamContext_t state = {
      .state = PARAM_STATE_1,
      .param_id = 0
  };

  FunctionContext_t context = {
      .state = &state,
      .input_len = 0,
      .output_buffer = output_buffer,
      .comm_interface = interface
  };

  uint16_t copy_len = input_len;
  if (copy_len >= MAX_COMM_IN_BUFFER_SIZE) {
    copy_len = MAX_COMM_IN_BUFFER_SIZE - 1;
  }
  memcpy(context.input, input, copy_len);
  context.input_len = copy_len;

  if (strstr(context.input, START_SOME_SEQUENCE) != NULL) {
    return importCore(&context, "", START_SOME_SEQUENCE, END_SOME_SEQUENCE,
        true, num_some_parameters);
  }

  if (strstr(context.input, START_ALL_SEQUENCE) != NULL) {
    return importCore(&context, "", START_ALL_SEQUENCE, END_ALL_SEQUENCE,
        false, 0);
  }

  COMM_TransmitHmiLine(HMI_TAG_ERROR, "CFG_IMPORT_UNKNOWN_SEQUENCE", interface);
  return false;
}

/* Private function definitions ----------------------------------------------*/

// Serializes and transmits one parameter. Returns false if the parameter
// could not be exported (unknown type, getter failure, or unhandled case)
static bool exportOneParameter(FunctionContext_t* context, ParamIds_t id)
{
  ParamType_t param_type;

  // Dont throw an error since some parameters are to be setup later
  if (Param_IsInitialized(id) == false) return true;

  if (Param_GetParamType(id, &param_type) == false) {
    return false;
  }

  uint16_t buffer_index = 0;
  buffer_index += sprintf((char*) context->output_buffer, "%hu-", id);

  switch (param_type) {
    case PARAM_TYPE_UINT8: {
      uint8_t value;
      if (Param_GetUint8(id, &value) == false) return false;
      buffer_index += sprintf((char*) &context->output_buffer[buffer_index], "%hu", value);
      break;
    }
    case PARAM_TYPE_INT8: {
      int8_t value;
      if (Param_GetInt8(id, &value) == false) return false;
      buffer_index += sprintf((char*) &context->output_buffer[buffer_index], "%hd", value);
      break;
    }
    case PARAM_TYPE_UINT16: {
      uint16_t value;
      if (Param_GetUint16(id, &value) == false) return false;
      buffer_index += sprintf((char*) &context->output_buffer[buffer_index], "%u", value);
      break;
    }
    case PARAM_TYPE_INT16: {
      int16_t value;
      if (Param_GetInt16(id, &value) == false) return false;
      buffer_index += sprintf((char*) &context->output_buffer[buffer_index], "%d", value);
      break;
    }
    case PARAM_TYPE_UINT32: {
      uint32_t value;
      if (Param_GetUint32(id, &value) == false) return false;
      buffer_index += sprintf((char*) &context->output_buffer[buffer_index], "%lu", value);
      break;
    }
    case PARAM_TYPE_INT32: {
      int32_t value;
      if (Param_GetInt32(id, &value) == false) return false;
      buffer_index += sprintf((char*) &context->output_buffer[buffer_index], "%ld", value);
      break;
    }
    case PARAM_TYPE_FLOAT: {
      float value;
      if (Param_GetFloat(id, &value) == false) return false;
      buffer_index += sprintf((char*) &context->output_buffer[buffer_index], "%f", value);
      break;
    }
    case PARAM_TYPE_ENUM: {
      uint8_t value;
      if (Param_GetEnum(id, &value) == false) return false;
      buffer_index += sprintf((char*) &context->output_buffer[buffer_index], "%hu", value);
      break;
    }
    default:
      return false;
  }

  buffer_index += sprintf((char*) &context->output_buffer[buffer_index], DELIMITER);
  COMM_TransmitData(context->output_buffer, buffer_index, context->comm_interface);
  return true;
}

// Transmits the user-facing copy prompt, the start marker, and the delimiter
static void transmitExportHeader(FunctionContext_t* context,
    const char* start_marker, const char* end_marker)
{
  COMM_TransmitHmiLinef(HMI_TAG_STATUS, context->comm_interface,
      "CFG_EXPORT_BEGIN start=%s end=%s", start_marker, end_marker);
  COMM_TransmitData(start_marker, CALC_LEN, context->comm_interface);
  COMM_TransmitData(DELIMITER, CALC_LEN, context->comm_interface);
}

// Parses one "id-value" entry from *curr, sets the parameter, and advances
// *curr past the value. Returns false and transmits an error on any failure
static bool parseAndSetOneParameter(FunctionContext_t* context,
    const char** curr)
{
  char* end_ptr;
  ParamIds_t id = (ParamIds_t) strtoul(*curr, &end_ptr, 10);

  if (id >= NUM_PARAM) {
    COMM_TransmitHmiLinef(HMI_TAG_ERROR, context->comm_interface,
        "CFG_IMPORT_INVALID_ID id=%u", (unsigned int) id);
    return false;
  }

  if (*end_ptr != '-') {
    COMM_TransmitHmiLine(HMI_TAG_ERROR, "CFG_IMPORT_INVALID_FORMAT",
        context->comm_interface);
    return false;
  }

  *curr = end_ptr + 1;

  ParamType_t param_type;
  if (Param_GetParamType(id, &param_type) == false) {
    COMM_TransmitHmiLinef(HMI_TAG_ERROR, context->comm_interface,
        "CFG_IMPORT_UNKNOWN_ID id=%u", (unsigned int) id);
    return false;
  }

  bool set_result = false;
  switch (param_type) {
    case PARAM_TYPE_UINT8: {
      uint8_t value = (uint8_t) strtoul(*curr, &end_ptr, 10);
      set_result = Param_SetUint8(id, &value);
      break;
    }
    case PARAM_TYPE_INT8: {
      int8_t value = (int8_t) strtol(*curr, &end_ptr, 10);
      set_result = Param_SetInt8(id, &value);
      break;
    }
    case PARAM_TYPE_UINT16: {
      uint16_t value = (uint16_t) strtoul(*curr, &end_ptr, 10);
      set_result = Param_SetUint16(id, &value);
      break;
    }
    case PARAM_TYPE_INT16: {
      int16_t value = (int16_t) strtol(*curr, &end_ptr, 10);
      set_result = Param_SetInt16(id, &value);
      break;
    }
    case PARAM_TYPE_UINT32: {
      uint32_t value = (uint32_t) strtoul(*curr, &end_ptr, 10);
      set_result = Param_SetUint32(id, &value);
      break;
    }
    case PARAM_TYPE_INT32: {
      int32_t value = (int32_t) strtol(*curr, &end_ptr, 10);
      set_result = Param_SetInt32(id, &value);
      break;
    }
    case PARAM_TYPE_FLOAT: {
      float value = strtof(*curr, &end_ptr);
      set_result = Param_SetFloat(id, &value);
      break;
    }
    case PARAM_TYPE_ENUM: {
      uint8_t value = (uint8_t) strtoul(*curr, &end_ptr, 10);
      set_result = Param_SetEnum(id, &value);
      break;
    }
    default:
      COMM_TransmitHmiLinef(HMI_TAG_ERROR, context->comm_interface,
        "CFG_IMPORT_UNKNOWN_TYPE id=%u", (unsigned int) id);
      return false;
  }

  if (set_result != PARAM_SET_SUCCESS) {
    COMM_TransmitHmiLinef(HMI_TAG_ERROR, context->comm_interface,
      "CFG_IMPORT_SET_FAILED id=%u", (unsigned int) id);
    return false;
  }

  *curr = end_ptr;
  return true;
}

// Shared state machine for both import functions. Prompts in state 0 and
// parses in state 1. When check_count is true the number of imported
// parameters must equal expected_count or the import fails
static bool importCore(FunctionContext_t* context, const char* prompt,
    const char* start_marker, const char* end_marker,
    bool check_count, uint16_t expected_count)
{
  switch (context->state->state) {
    case PARAM_STATE_0:
      if (prompt != NULL && prompt[0] != '\0') {
        COMM_TransmitHmiPrompt(prompt, context->comm_interface);
      }
      context->state->state = PARAM_STATE_1;
      return true;

    case PARAM_STATE_1: {
      context->input[context->input_len] = '\0';

      const char* start = strstr(context->input, start_marker);
      if (start == NULL) {
        COMM_TransmitHmiLinef(HMI_TAG_ERROR, context->comm_interface,
            "CFG_IMPORT_START_NOT_FOUND start=%s", start_marker);
        context->state->state = PARAM_STATE_COMPLETE;
        return false;
      }
      start += strlen(start_marker);

      const char* end = strstr(start, end_marker);
      if (end == NULL) {
        COMM_TransmitHmiLinef(HMI_TAG_ERROR, context->comm_interface,
            "CFG_IMPORT_END_NOT_FOUND end=%s", end_marker);
        context->state->state = PARAM_STATE_COMPLETE;
        return false;
      }

      const char* curr = start;
      uint16_t params_imported = 0;

      while (curr < end) {
        while (curr < end &&
            (*curr == ',' || *curr == ' ' || *curr == '\r' || *curr == '\n')) {
          curr++;
        }

        if (curr >= end) break;

        if (parseAndSetOneParameter(context, &curr) == false) {
          context->state->state = PARAM_STATE_COMPLETE;
          return false;
        }
        params_imported++;
      }

      if (check_count && params_imported != expected_count) {
        COMM_TransmitHmiLinef(HMI_TAG_ERROR, context->comm_interface,
          "CFG_IMPORT_COUNT_MISMATCH imported=%u expected=%u",
          (unsigned int) params_imported, (unsigned int) expected_count);
        context->state->state = PARAM_STATE_COMPLETE;
        return false;
      }

        COMM_TransmitHmiLinef(HMI_TAG_STATUS, context->comm_interface,
          "CFG_IMPORT_DONE imported=%u",
          (unsigned int) params_imported);
      context->state->state = PARAM_STATE_COMPLETE;
        return true;
    }

    default:
      context->state->state = PARAM_STATE_COMPLETE;
      return false;
  }
}
