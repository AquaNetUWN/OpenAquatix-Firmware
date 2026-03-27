/*
 * cfg_import_export.h
 *
 *  Created on: Apr 17, 2025
 *      Author: ericv
 * 
 * Copyright (c) 2025 OpenAquatix Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef CFG_CFG_IMPORT_EXPORT_H_
#define CFG_CFG_IMPORT_EXPORT_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"
#include "comm_menu_system.h"
#include <stdbool.h>


/* Private includes ----------------------------------------------------------*/



/* Exported types ------------------------------------------------------------*/



/* Exported constants --------------------------------------------------------*/



/* Exported macro ------------------------------------------------------------*/



/* Exported functions prototypes ---------------------------------------------*/

/**
 * @brief Exports configuration options in text format to HMI
 *
 * Exports predetermined list of configuration values to the HMI by first
 * converting the data to human readable text. Values exported by this function
 * are only identifiable by their parameter id so it is made for peer-peer rather
 * than debugging or an overall view
 *
 * @param context Gives a buffer and interface context
 *
 * @return true if successful, false otherwise
 *
 * @note Does not inform users of errors internally
 * @see ImportExport_ImportConfiguration
 */
bool ImportExport_ExportConfiguration(FunctionContext_t* context);

/**
 * @brief Imports configuration values exported from another device
 *
 * 2 stages: The first stage prompts the user to input a configuration string.
 * The second stage parses the input and updates the input parameters.
 *
 * @param context Gives an output buffer, the string to parse, the string to
 *        parse's length, and the interface context. Changes the context's state
 *
 * @note The number of parameters imported and exported must be the same.
 * @see ImportExport_ExportConfiguration
 */
bool ImportExport_ImportConfiguration(FunctionContext_t* context);

/**
 * @brief Imports configuration values from a single text buffer
 *
 * Parses the same START...END export blob accepted by the interactive import
 * menu entry and applies the contained parameter values immediately.
 *
 * @param input Serialized configuration text
 * @param input_len Length of the serialized text
 * @param output_buffer Scratch buffer used for emitted status or error text
 * @param interface Target interface for emitted status or error text
 *
 * @return true if import completed successfully, false otherwise
 */
bool ImportExport_ImportConfigurationText(const char* input, uint16_t input_len,
                                          uint8_t* output_buffer,
                                          CommInterface_t interface);

/* Private defines -----------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif /* CFG_CFG_IMPORT_EXPORT_H_ */
