/*
 * comm_main.h
 *
 *  Created on: Feb 2, 2025
 *      Author: ericv
 * 
 * Copyright (c) 2025 OpenAquatix Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef __COMM_MAIN_H_
#define __COMM_MAIN_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/

#include "stm32h7xx_hal.h"
#include "mac_channel_reports.h"
#include "mess_main.h"
#include <stdbool.h>

/* Private includes ----------------------------------------------------------*/


/* Exported types ------------------------------------------------------------*/

#define MAX_COMM_IN_BUFFER_SIZE   512
#define MAX_COMM_OUT_BUFFER_SIZE  1024

#define CALC_LEN                  0 // A length of 0 makes the function call strlen

#define WITHDRAW_CHAR             '\e' // Character that when entered prompts back navigation

typedef enum {
  NEW_CONTENT,
  DATA_READY,
  NO_CHANGE
} RxState_t;

typedef enum {
  COMM_USB,
  COMM_UART,
  COMM_BOTH
} CommInterface_t;

typedef enum {
  HMI_TAG_MENU,
  HMI_TAG_PROMPT,
  HMI_TAG_STATUS,
  HMI_TAG_ERROR,
  HMI_TAG_NOTIFY,
  HMI_TAG_MSG_RX
} HmiTag_t;

typedef enum {
  HMI_INPUT_CONTEXT_MENU,
  HMI_INPUT_CONTEXT_FUNCTION
} HmiInputContext_t;

typedef enum {
  TELEMETRY_GROUP_NONE,
  TELEMETRY_GROUP_ALL,
  TELEMETRY_GROUP_TEMP,
  TELEMETRY_GROUP_POWER,
  TELEMETRY_GROUP_ELECTRICAL,
  TELEMETRY_GROUP_ENV
} TelemetryGroup_t;

typedef struct {
  bool enabled;
  TelemetryGroup_t group;
  uint32_t period_ms;
  CommInterface_t interface;
} TelemetrySubscriptionStatus_t;

typedef struct {
  uint8_t buffer[MAX_COMM_IN_BUFFER_SIZE];
  uint16_t length;
  uint16_t index;
  bool contents_changed;
  bool data_ready;
  CommInterface_t source;
} CommBuffer_t;

/* Exported constants --------------------------------------------------------*/



/* Exported macro ------------------------------------------------------------*/



/* Exported functions prototypes ---------------------------------------------*/

/**
 * @brief Entry point for the communication interface task
 *
 * Initializes communication subsystems (USB and DAU), registers with the parameter system,
 * and manages the HMI. This task handles menu navigation, command processing, and message
 * routing between different communication interfaces.
 *
 * @param argument Task parameter (unused, required by RTOS task signature)
 *
 * @note This function never returns and should be started as an RTOS task
 */
void COMM_StartTask(void *argument);

/**
 * @brief Transmits data through the specified communication interface(s)
 *
 * Sends data through USB, UART, or both interfaces based on the interface parameter.
 *
 * @param data Pointer to the data buffer to transmit
 * @param data_len Length of data in bytes (if 0, strlen is used to calculate length)
 * @param interface Target communication interface (COMM_USB, COMM_UART, or COMM_BOTH)
 */
void COMM_TransmitData(const void *data, uint32_t data_len, CommInterface_t interface);

/**
 * @brief Returns whether the session-scoped tagged HMI mode is enabled
 *
 * @return true when tagged output is enabled, false otherwise
 */
bool COMM_IsTagModeEnabled(void);

/**
 * @brief Enables or disables the session-scoped tagged HMI mode
 *
 * @param enabled true to enable tagged mode, false to disable it
 */
void COMM_SetTagMode(bool enabled);

/**
 * @brief Returns whether HMI prompts are enabled for the current session
 *
 * @return true when prompts are enabled, false otherwise
 */
bool COMM_IsPromptEnabled(void);

/**
 * @brief Enables or disables HMI prompts for the current session
 *
 * @param enabled true to enable prompts, false to disable them
 */
void COMM_SetPromptEnabled(bool enabled);

/**
 * @brief Transmits a logical HMI line using the current session formatting mode
 *
 * When tagged mode is enabled, the line is prefixed with the provided HMI tag.
 * Otherwise the text is transmitted as a plain line ending in CRLF.
 *
 * @param tag HMI tag to use when tagged mode is enabled
 * @param text Logical line contents without trailing CRLF
 * @param interface Target communication interface
 */
void COMM_TransmitHmiLine(HmiTag_t tag, const char* text, CommInterface_t interface);

/**
 * @brief printf-style variant of COMM_TransmitHmiLine()
 *
 * @param tag HMI tag to use when tagged mode is enabled
 * @param interface Target communication interface
 * @param format printf-style format string
 */
void COMM_TransmitHmiLinef(HmiTag_t tag, CommInterface_t interface, const char* format, ...);

/**
 * @brief Transmits a tagged machine-response line regardless of session mode
 *
 * @param tag HMI tag to use
 * @param text Logical line contents without trailing CRLF
 * @param interface Target communication interface
 */
void COMM_TransmitHmiMachineLine(HmiTag_t tag, const char* text, CommInterface_t interface);

/**
 * @brief printf-style variant of COMM_TransmitHmiMachineLine()
 *
 * @param tag HMI tag to use
 * @param interface Target communication interface
 * @param format printf-style format string
 */
void COMM_TransmitHmiMachineLinef(HmiTag_t tag, CommInterface_t interface, const char* format, ...);

/**
 * @brief Transmits raw COMM-owned text in raw mode or tagged logical lines in tag mode
 *
 * This is intended for existing COMM text paths that still build CRLF-delimited
 * strings. In tagged mode, each non-empty line is emitted with the provided
 * tag. In raw mode, the text is transmitted unchanged.
 *
 * @param tag HMI tag to use when tagged mode is enabled
 * @param text Text buffer that may contain CR/LF-delimited lines
 * @param interface Target communication interface
 */
void COMM_TransmitTaggedText(HmiTag_t tag, const char* text, CommInterface_t interface);

/**
 * @brief printf-style variant of COMM_TransmitTaggedText()
 *
 * @param tag HMI tag to use when tagged mode is enabled
 * @param interface Target communication interface
 * @param format printf-style format string
 */
void COMM_TransmitTaggedTextf(HmiTag_t tag, CommInterface_t interface, const char* format, ...);

/**
 * @brief Transmits a logical HMI prompt
 *
 * In tagged mode, prompts are emitted as a single line ending in the exact
 * suffix " > ". In raw mode, the text is emitted as-is followed by CRLF.
 *
 * @param text Prompt text without trailing CRLF
 * @param interface Target communication interface
 */
void COMM_TransmitHmiPrompt(const char* text, CommInterface_t interface);

/**
 * @brief printf-style variant of COMM_TransmitHmiPrompt()
 *
 * @param interface Target communication interface
 * @param format printf-style format string
 */
void COMM_TransmitHmiPromptf(CommInterface_t interface, const char* format, ...);

/**
 * @brief Enables or disables machine-readable RX event streaming
 *
 * @param enabled true to enable the stream, false to disable it
 * @param interface Target interface for streamed events when enabling
 */
void COMM_SetHostRxSubscription(bool enabled, CommInterface_t interface);

/**
 * @brief Enables or disables machine-readable channel sensing event streaming
 *
 * @param enabled true to enable the stream, false to disable it
 * @param interface Target interface for streamed events when enabling
 */
void COMM_SetHostSenseSubscription(bool enabled, CommInterface_t interface);

/**
 * @brief Returns whether machine-readable RX event streaming is enabled
 *
 * @return true if RX streaming is enabled
 */
bool COMM_IsHostRxSubscriptionEnabled(void);

/**
 * @brief Returns whether machine-readable sensing event streaming is enabled
 *
 * @return true if sensing streaming is enabled
 */
bool COMM_IsHostSenseSubscriptionEnabled(void);

/**
 * @brief Emits a machine-readable RX event if the host RX stream is enabled
 *
 * @param msg Received message metadata and payload
 */
void COMM_ReportHostRxEvent(const Message_t* msg);

/**
 * @brief Emits a machine-readable sensing event if the sensing stream is enabled
 *
 * @param report Completed channel report metadata
 */
void COMM_ReportHostSenseEvent(const ChannelReport_t* report);

/**
 * @brief Emits a machine-readable telemetry snapshot
 *
 * @param interface Target interface
 * @param group Telemetry group to emit
 * @return true if the snapshot was emitted successfully
 */
bool COMM_TransmitTelemetrySnapshot(CommInterface_t interface, TelemetryGroup_t group);

/**
 * @brief Enables, disables, or reconfigures the periodic telemetry stream
 *
 * @param enabled true to enable the stream, false to disable it
 * @param group Telemetry group to stream when enabled
 * @param period_ms Stream cadence in milliseconds when enabled
 * @param interface Target interface for streamed events when enabling
 * @return true if the subscription state was applied successfully
 */
bool COMM_SetHostTelemetrySubscription(bool enabled, TelemetryGroup_t group, uint32_t period_ms,
                                       CommInterface_t interface);

/**
 * @brief Returns the current telemetry stream subscription state
 *
 * @param status Destination for the current subscription state
 * @return true if status was written successfully
 */
bool COMM_GetHostTelemetrySubscriptionStatus(TelemetrySubscriptionStatus_t* status);

/* Private defines -----------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif /* __COMM_MAIN_H_ */
