# COMM Command ICD

## Scope

This document specifies the `:` command system implemented in
`Application/Src/COMM/comm_commands.c` as of the current firmware state.

It documents the machine-facing command parser, command set, argument rules,
response patterns, and known implementation constraints.

## Transport and Session Model

- Commands are carried over the existing COMM HMI transport on USB or UART.
- A command is only recognized when the input line begins with `:`, after any
  leading whitespace.
- Command handling is session-scoped in the sense that `:tag`, `:prompt`,
  `:rxsub`, and `:sense` affect the current COMM session/interface state.
- Command execution suppresses menu redraw by default.

## HMI Tagging System

The COMM layer supports two output formatting modes:

- raw mode
- tagged mode

Raw mode is intended for interactive humans and preserves older text-style HMI
behavior. Tagged mode is intended for host parsing and turns logical HMI output
into line-oriented records prefixed with a tag class.

### Tag Classes

The current firmware defines these HMI tags:

- `MENU`
- `PROMPT`
- `STATUS`
- `ERROR`
- `NOTIFY`
- `MSG_RX`

These appear on the wire as bracketed prefixes:

```text
[STATUS] OK :status ...
[ERROR] Invalid route: invalid
[PROMPT] Select option > 
[MSG_RX] EVENT protocol=custom ...
```

### Raw Mode

When tagged mode is disabled:

- `COMM_TransmitHmiLine()` emits `<text>\r\n`
- `COMM_TransmitHmiPrompt()` emits `<text>\r\n`
- `COMM_TransmitTaggedText()` emits its input buffer unchanged
- legacy menu text and CFG text remain plain text

This means raw mode is not uniformly machine-parseable. Some command handlers
still force tagged output for machine responses as described below.

### Tagged Mode

When tagged mode is enabled:

- `COMM_TransmitHmiLine()` emits `[TAG] <text>\r\n`
- `COMM_TransmitHmiPrompt()` emits `[PROMPT] <text> > `
- `COMM_TransmitTaggedText()` splits CR/LF-delimited text into logical lines
  and emits each non-empty line as `[TAG] <line>\r\n`
- blank lines in legacy text are discarded rather than emitted as empty tagged
  records

This allows older menu/config code that still builds multi-line text buffers to
participate in the tagged HMI without rewriting every call site.

### Prompt Interaction

Prompts are handled specially:

- in tagged mode, a prompt is emitted without a trailing CRLF
- the exact suffix is ` > `
- if another tagged line is emitted while a prompt is still active, COMM first
  emits `\r\n` to terminate the prompt line, then emits the new tagged record
- in raw mode, prompts are normal CRLF-terminated text lines

This matters for host parsers: a `[PROMPT]` line may be the active tail of the
stream until another line forces prompt finalization.

### Forced Machine Lines

Some outputs are always tagged, even when `:tag off` is in effect.

These use `COMM_TransmitHmiMachineLine()` /
`COMM_TransmitHmiMachineLinef()` and currently include:

- command success responses
- command error responses
- host RX event stream records
- host sensing event stream records

This guarantees that the machine command surface remains parseable even if the
session is otherwise operating in raw mode.

### Legacy Text Paths

Not every subsystem emits machine-native key/value responses.

- menu rendering and many menu functions still generate human-readable text
- `importcfg` reuses CFG import code that emits legacy plain text
- when tagged mode is on, these paths are adapted by
  `COMM_TransmitTaggedText()` into tagged logical lines
- when tagged mode is off, they remain plain text

As a result, tag presence alone does not guarantee a strict key/value machine
schema. It only guarantees a tag class and line boundary.

## Parsing Rules

### Command Name

- Command names are case-insensitive.
- Leading whitespace before `:` is ignored.
- Whitespace between `:` and the command name is ignored.

Examples:

- `:status`
- `  :STATUS`
- `:Tx route=transducer payload=QQ==`

### Arguments

Arguments are tokenized by spaces with simple shell-like behavior:

- quoted tokens with `'...'` or `"..."` are supported
- backslash escapes the next character
- key/value arguments use `key=value`
- option keys are case-insensitive
- many option values are also case-insensitive

Examples:

- `:help tx`
- `:tx route=feedback payload=QQ==`
- `:importcfg "START,1-80,2-12000,END"`

## Output Conventions

### Command Responses

Most command handlers use machine lines:

- success: `[STATUS] OK ...`
- failure: `[ERROR] ...`

These are emitted with `COMM_TransmitHmiMachineLine*()`, so they are tagged
even if normal tag mode is disabled.

### Tagging and Commands

- `:tag on` enables tagged formatting for normal HMI output
- `:tag off` returns normal HMI output to raw text formatting
- command responses themselves remain tagged in both cases because they use the
  forced machine-line path
- host event streams enabled by `:rxsub on` or `:sense on` also remain tagged
  in both cases

In practice, `:tag on` mainly affects:

- menu screens
- prompts
- human-readable print output
- legacy multi-line COMM/CFG text

### Exceptions

`importcfg` reuses the existing configuration import implementation in CFG.
That import path emits its own legacy text via `COMM_TransmitData()`:

- success text such as `Successfully imported <N> parameters`
- parser errors such as `Error: start sequence not found`

After a successful import, the command handler also emits:

- `[STATUS] OK :importcfg`

This means `importcfg` currently mixes the machine command surface with the
older text output path.

## Command Reference

### `:help`

Usage:

```text
:help [command]
```

Description:

- Entry point for command discovery on the modem.
- Without an argument, it enumerates the currently compiled command table.
- With a command name, it returns the command name, the built-in description
  string, and the usage string from `comm_commands.c`.

Behavior:

- no argument:
  - emits `Available commands:`
  - emits one line per command in the form `:<name> - <description>`
- one argument:
  - looks up the command name case-insensitively
  - accepts either `help tx` or `help :tx`
  - emits three lines:
    - `Command: :<name>`
    - `Description: <description>`
    - `Usage: :<usage>`
- unknown command:
  - emits `[ERROR] Unknown command: :<name>`

Notes:

- The help text is generated directly from the command table, so it reflects
  the compiled firmware rather than an external document.

### `:tag`

Usage:

```text
:tag on|off
```

Description:

- Controls whether normal COMM HMI output is emitted in raw text form or in
  bracket-tagged line form.
- This affects menu screens, human-readable notifications, print output, and
  other non-forced HMI text on the current session.

Behavior:

- `on` enables tagged output mode for the current session
- `off` disables tagged output mode for the current session
- disabling tag mode also clears any active tagged prompt state
- success response:
  - `[STATUS] OK :tag on`
  - `[STATUS] OK :tag off`

Notes:

- Command responses and host event streams remain tagged even when `:tag off`
  is selected, because they use the forced machine-line path.

### `:prompt`

Usage:

```text
:prompt on|off
```

Description:

- Controls whether COMM emits prompts for interactive menu usage on the current
  session.
- This is primarily relevant for human-driven HMI sessions rather than command
  automation.

Behavior:

- `on` enables prompts
- `off` disables prompts
- disabling prompts also clears any currently active prompt state
- success response:
  - `[STATUS] OK :prompt on`
  - `[STATUS] OK :prompt off`

Notes:

- In tagged mode, prompts are emitted as `[PROMPT] <text> > `.
- In raw mode, prompts are emitted as normal CRLF-terminated text lines.

### `:print`

Usage:

```text
:print on|off
```

Description:

- Enables or disables the firmware’s human-readable printing of received
  messages.
- This command writes `PARAM_PRINT_ENABLED`, which controls whether decoded RX
  traffic is rendered through the traditional COMM print path.

Behavior:

- `on` sets `PARAM_PRINT_ENABLED = 1`
- `off` sets `PARAM_PRINT_ENABLED = 0`
- success response:
  - `[STATUS] OK :print on`
  - `[STATUS] OK :print off`
- failure response:
  - `[ERROR] Failed to set printing received messages`

Operational effect:

- When enabled, received traffic may be printed in human-readable form through
  the existing `comm_print.c` path.
- When disabled, that print path is suppressed.

Notes:

- `:print` does not control the machine-readable host RX stream used by
  `:rxsub`; those are independent paths.

### `:mode`

Usage:

```text
:mode hostmac on|off
```

Description:

- Selects whether the firmware runs in host-controlled MAC mode.
- This is the mode gate for Pi-owned MAC scheduling and host-controlled delayed
  transmission via `:txat`.

Behavior:

- `on` sets `PARAM_MAC = MAC_PROTOCOL_HOST_CONTROL`
- `off` sets `PARAM_MAC = MAC_PROTOCOL_NONE`
- success response:
  - `[STATUS] OK :mode hostmac on`
  - `[STATUS] OK :mode hostmac off`
- failure response:
  - `[ERROR] Failed to update MAC mode`

Operational effect:

- In `hostmac` mode, the MAC layer bypasses its local contention/arbitration
  policy and accepts host-scheduled transmissions.
- Turning host mode off currently selects `MAC_PROTOCOL_NONE`; it does not
  restore a previous CSMA/CA setting automatically.

### `:time`

Usage:

```text
:time
```

Description:

- Returns the current device-local timing references that a host uses to map
  its own timebase onto the MCU timebase.
- This is the synchronization primitive used before issuing `:txat`.

Behavior:

- takes no arguments
- returns current device scheduling clocks
- success response format:

```text
[STATUS] OK :time tick_ms=<u64> cyccnt=<u32>
```

Fields:

- `tick_ms`: `HAL_AbsoluteTimestamp()`
- `cyccnt`: `DWT->CYCCNT`

Notes:

- `tick_ms` is a coarse absolute timestamp in milliseconds.
- `cyccnt` is the high-resolution cycle counter used by the delayed-TX path.

### `:tx`

Usage:

```text
:tx route=<transducer|feedback> payload=<base64>
```

Description:

- Queues an immediate transmission request into the regular TX queue.
- This is the simple non-scheduled command path for sending a base64 payload
  out either the transducer path or the feedback path.

Accepted payload aliases:

- `payload=...`
- `data=...`
- `b64=...`

Behavior:

- decodes base64 payload
- builds an immediate `Message_t` with:
  - `protocol = PROTOCOL_CUSTOM`
  - `data_type = BITS`
  - `preamble.message_type = BITS`
  - `timestamp = osKernelGetTickCount()`
  - `type = MSG_TRANSMIT_TRANSDUCER` or `MSG_TRANSMIT_FEEDBACK` based on `route`
- enqueues the message into `regular_tx_queue`

Operational effect:

- The payload is treated as a custom `BITS` message.
- No additional message typing or JANUS support is available on this command.
- Transmission timing is immediate/best-effort via the normal TX queue rather
  than host-scheduled.

Success response:

```text
[STATUS] OK :tx route=<transducer|feedback>
```

Failure conditions:

- missing `route` or payload
- invalid `route`
- unknown option key
- malformed non-`key=value` argument
- base64 decode failure
- decoded payload exceeds `PACKET_DATA_MAX_LENGTH_BYTES`
- no space / queue failure

### `:range`

Usage:

```text
:range [route=transducer|feedback]
```

Description:

- Queues an immediate ranging request through the same MESS event-driven path
  used by the TX/RX menu.
- This is a dedicated command-surface shortcut for immediate ranging; it does
  not introduce a separate ranging subsystem or result stream.

Behavior:

- takes zero or one argument
- no argument defaults to `route=transducer`
- `route=feedback` selects the feedback network
- does not require host MAC mode
- enqueues `MESS_REQUEST_RANGE_TRANSDUCER` or `MESS_REQUEST_RANGE_FEEDBACK`

Success response:

```text
[STATUS] OK :range route=transducer
[STATUS] OK :range route=feedback
```

Failure conditions:

- malformed or extra arguments
- `route` missing its value
- invalid `route`
- ranging request could not be queued into the existing event path

Operational effect:

- A successful command only means the request was accepted into the existing
  ranging path.
- Ranging completion remains asynchronous and uses the normal RX handling path.
- When `:rxsub on` is enabled, completed ranging responses continue to emit the
  normal `[MSG_RX] EVENT ... range_m=<float> ...` line.
- When `:print on` is enabled, the legacy print path continues to display the
  human-readable range output.
- Existing async ranging failure text is unchanged.

### `:txat`

Usage:

```text
:txat route=<transducer|feedback> protocol=<custom|janus> payload=<base64> tx_cyccnt=<u32> [type=<custom_type>] [janus_type=sms] [modem_id=<u16>] [is_mobile=<0|1|true|false>] [reservation_time_10ms=<u16>] [schedule_flag=<u16>] [destination_id=<u16>] [tx_rx_capable=<0|1|true|false>] [can_forward=<0|1|true|false>] [coding=<u16>] [encryption=<u16>]
```

Required fields:

- `route`
- `protocol`
- `payload`
- `tx_cyccnt`
- `type` or `custom_type` when `protocol=custom`

Optional fields:

- `janus_type=sms`
- preamble overrides:
  - `modem_id`
  - `is_mobile`
  - `reservation_time_10ms`
  - `schedule_flag`
  - `destination_id`
  - `tx_rx_capable`
  - `can_forward`
  - `coding`
  - `encryption`

Description:

- Schedules a future transmission at a specific device `CYCCNT` value.
- This is the primary host-controlled TX path for Pi-owned MAC operation.
- The command constructs a `Message_t`, marks it delayed, and hands it to the
  MAC host-scheduler path rather than directly queueing it to `MESS`.

Behavior:

- only accepted when host MAC mode is enabled
- payload is base64-decoded into `Message_t.data`
- `delay = true`
- `delay_cyccnt = tx_cyccnt`
- request is stored in the MAC host scheduling slot until it is close enough to
  be handed to `MESS`

Operational effect:

- `route=transducer` produces `MSG_TRANSMIT_TRANSDUCER`
- `route=feedback` produces `MSG_TRANSMIT_FEEDBACK`
- `protocol=custom` requires `type=` or `custom_type=` and copies that into
  `msg.data_type` and the preamble message type field
- `protocol=janus` currently supports only `janus_type=sms`
- optional preamble fields are copied into the outgoing message and marked
  valid so the host can override modem metadata when needed

Success response:

```text
[STATUS] OK :txat request_id=<u32> protocol=<custom|janus> tx_cyccnt=<u32>
```

Failure conditions:

- host MAC mode disabled
- missing required fields
- invalid enum/boolean/integer field
- unknown or malformed option key
- base64 decode failure
- unsupported `janus_type`
- `protocol=custom` without `type=<custom_type>`
- scheduling rejected because:
  - timestamp too close
  - timestamp in the past / beyond safe wrap window
  - another scheduled TX is already pending

Notes:

- The current implementation supports only one pending host-scheduled TX at a
  time.
- A successful response returns a `request_id` that can be used with
  `:cancel_tx`.

### `:cancel_tx`

Usage:

```text
:cancel_tx <request_id>
```

Description:

- Cancels the one pending host-scheduled transmission tracked by the MAC
  host-control path.
- This command is intended to withdraw a scheduled `:txat` request before it
  has been handed off to `MESS`.

Behavior:

- takes a numeric `request_id`
- rejects `0` and non-numeric values
- cancels the pending host-scheduled TX if it has not yet been handed off to
  `MESS`

Success response:

```text
[STATUS] OK :cancel_tx request_id=<u32>
```

Failure response:

```text
[ERROR] No pending scheduled transmission with id=<u32>
```

Notes:

- Once a scheduled message has already been handed from the MAC host slot into
  the lower transmit path, `:cancel_tx` can no longer retract it.

### `:rxsub`

Usage:

```text
:rxsub on|off
```

Description:

- Enables or disables the machine-readable RX event stream emitted when the
  modem receives and decodes messages.
- This stream is intended for host automation and host-owned MAC logic.

Behavior:

- `on` enables RX event streaming and records the current COMM interface as the
  destination for future RX events
- `off` disables RX event streaming

Success response:

```text
[STATUS] OK :rxsub on
[STATUS] OK :rxsub off
```

Notes:

- The subscription is global in effect but remembers the interface that last
  enabled it.
- Re-enabling on a different interface moves the stream to that interface.

### `:sense`

Usage:

```text
:sense on|off
```

Description:

- Enables or disables the machine-readable channel sensing telemetry stream.
- This stream exposes background-noise / PSD measurements to the host without
  letting the firmware MAC use them for local access control in host mode.

Behavior:

- `on` enables sensing event streaming and records the current COMM interface as
  the destination for future sense events
- `off` disables sensing event streaming

Success response:

```text
[STATUS] OK :sense on
[STATUS] OK :sense off
```

Notes:

- Like `:rxsub`, the enabled subscription is tied to the last interface that
  turned it on.

### `:status`

Usage:

```text
:status
```

Description:

- Returns a compact machine-readable snapshot of the current host-control state.
- This is the primary diagnostic command for checking whether host mode,
  subscriptions, queues, and pending scheduled TX state are aligned with host
  expectations.

Behavior:

- takes no arguments
- queries `MAC_GetHostStatus()`
- reports current host-MAC and subscription state

Success response format:

```text
[STATUS] OK :status mode=<hostmac|local> rxsub=<on|off> sense=<on|off> mac_regular_tx_depth=<u32> mac_emergency_tx_depth=<u32> mess_tx_depth=<u32> pending_scheduled_tx=<yes|no> request_id=<u32> tx_cyccnt=<u32> telemetrysub=<on|off> telemetry_group=<all|temp|power|electrical|env|none> telemetry_period_ms=<u32>
```

Field meanings:

- `mode`: `hostmac` when host-controlled MAC is enabled, otherwise `local`
- `rxsub`: whether the host RX stream is enabled
- `sense`: whether the host sense stream is enabled
- `mac_regular_tx_depth`: depth of the MAC regular TX queue
- `mac_emergency_tx_depth`: depth of the MAC emergency TX queue
- `mess_tx_depth`: depth of the `MESS` TX queue
- `pending_scheduled_tx`: whether a host-scheduled TX is currently parked in
  the MAC host slot
- `request_id`: current scheduled request id, or `0` when none is pending
- `tx_cyccnt`: target scheduled `CYCCNT`, or `0` when none is pending
- `telemetrysub`: whether periodic telemetry streaming is enabled
- `telemetry_group`: current telemetry stream group, or `none` when disabled
- `telemetry_period_ms`: current telemetry stream period, or `0` when disabled

### `:telemetry`

Usage:

```text
:telemetry [all|temp|power|electrical|env]
```

Description:

- Returns a compact machine-readable telemetry snapshot for one logical group or
  for all supported groups at once.
- Always includes device timing references and per-group ready flags so hosts
  can distinguish unavailable metrics from valid zero-valued measurements.

Behavior:

- takes zero or one argument
- default group is `all`
- valid groups are `all`, `temp`, `power`, `electrical`, and `env`
- emits one machine-readable status line
- omits numeric fields for any group whose corresponding ready flag is `no`

Success response format:

```text
[STATUS] OK :telemetry group=<all|temp|power|electrical|env> tick_ms=<u64> cyccnt=<u32> temp_ready=<yes|no> power_ready=<yes|no> electrical_ready=<yes|no> env_ready=<yes|no> ...
```

Possible numeric fields:

- `temp`: `tj_current_c`, `tj_peak_c`, `tj_avg_c`
- `power`: `power_latest_w`, `power_peak_w`, `power_avg_w`, `energy_since_boot_j`
- `electrical`: `voltage_latest_v`, `voltage_min_v`, `voltage_max_v`,
  `voltage_avg_v`, `current_latest_a`, `current_min_a`, `current_max_a`,
  `current_avg_a`
- `env`: `ambient_temp_c`, `pressure_hpa`

### `:telemetrysub`

Usage:

```text
:telemetrysub on|off [all|temp|power|electrical|env] [period_ms=<u32>]
```

Description:

- Enables or disables periodic machine-readable telemetry streaming.
- This is a COMM-owned host stream similar to `:rxsub` and `:sense`, but it is
  timer-driven rather than message-driven.

Behavior:

- `on` enables telemetry streaming on the current interface
- default group is `all`
- default `period_ms` is `1000`
- valid `period_ms` range is `100..60000`
- `off` disables telemetry streaming and clears the active stream state
- the enabled subscription is tied to the last interface that turned it on

Success response examples:

```text
[STATUS] OK :telemetrysub on group=all period_ms=1000
[STATUS] OK :telemetrysub on group=power period_ms=250
[STATUS] OK :telemetrysub off
```

### `:errorlog`

Usage:

```text
:errorlog
```

Description:

- Returns the retained error log in machine-readable form.
- The response is a summary line followed by zero or more entry lines.
- Entries are sorted by `reset_count`, then by `timestamp_ms`.

Success response format:

```text
[STATUS] OK :errorlog count=<u32> current_tick_ms=<u64> current_reset_count=<u32>
[STATUS] ENTRY :errorlog index=<u32> timestamp_ms=<u64> reset_count=<u32> error_code=<u32> severity=<token> file=<token> task=<token> line=<u32> occurrences=<u32> description_b64=<base64>
```

Field meanings:

- `count`: number of retained entries returned after the summary line
- `index`: zero-based index within the sorted snapshot
- `severity`: token-safe lower-case representation of the current severity text
- `description_b64`: base64-encoded human-readable error description

### `:importcfg`

Usage:

```text
:importcfg <START,...,END>
```

Description:

- Imports a serialized configuration blob using the same parser and data format
  as the menu-based configuration import path.
- This command exists so host automation can apply the same `START,...,END`
  config payload without driving the interactive menu tree.

Behavior:

- accepts the same configuration blob used by menu path `1 > 1 > 15`
- reuses the same CFG import parser as the menu importer
- applies all imported parameters immediately
- currently expects a single argument after tokenization, so quoting the full
  blob is recommended when sending it through shells or test harnesses that may
  otherwise split on spaces

Success text:

- legacy CFG text:
  - `Successfully imported <N> parameters`
- then command completion line:
  - `[STATUS] OK :importcfg`

Failure text examples:

- `Error: start sequence not found`
- `Error: end sequence not found`
- `Invalid id received!`
- `Invalid format!`
- `Unknown ID: <id>`
- `Unknown parameter type for ID <id>`
- `Failed to set parameter with ID <id>`

Notes:

- Import success currently requires the number of parsed parameters to match the
  import/export parameter list exactly.
- This command emits legacy CFG text in addition to the final machine status
  line, so consumers should handle mixed output.

### `:config`

Usage:

```text
:config [get|set <parameter_name> [value]]
```

Current state:

- Placeholder for future machine access to individual configuration parameters.
- The command name, description, and usage text exist, but no get/set behavior
  is implemented yet.
- always returns:

```text
[ERROR] Config command not implemented yet
```

## Host Event Streams

### RX Events

Enabled by:

```text
:rxsub on
```

Current emitted format:

```text
[MSG_RX] EVENT protocol=<custom|janus> route=<transducer|feedback> timestamp_ms=<u32> rx_cyccnt=<u32> length_bits=<u16> error=<0|1> snr=<float> doppler_mps=<float> ...
```

Possible extra fields:

- `data_type=<custom_type>`
- `janus_type=sms`
- `modem_id`
- `is_mobile`
- `reservation_time_10ms`
- `schedule_flag`
- `destination_id`
- `tx_rx_capable`
- `can_forward`
- `coding`
- `encryption`
- `range_m`
- `payload_b64=<base64>`

For dedicated ranging:

- `:range` results do not use a new event type
- completed ranging responses continue to surface here as normal RX events with
  `range_m`

### Sensing Events

Enabled by:

```text
:sense on
```

Current emitted format:

```text
[NOTIFY] EVENT sense timestamp_ms=<u64> cyccnt=<u32> psd=<float>
```

### Telemetry Events

Enabled by:

```text
:telemetrysub on [all|temp|power|electrical|env] [period_ms=<u32>]
```

Current emitted format:

```text
[NOTIFY] EVENT telemetry group=<all|temp|power|electrical|env> tick_ms=<u64> cyccnt=<u32> temp_ready=<yes|no> power_ready=<yes|no> electrical_ready=<yes|no> env_ready=<yes|no> ...
```

Possible numeric fields:

- `tj_current_c`
- `tj_peak_c`
- `tj_avg_c`
- `power_latest_w`
- `power_peak_w`
- `power_avg_w`
- `energy_since_boot_j`
- `voltage_latest_v`
- `voltage_min_v`
- `voltage_max_v`
- `voltage_avg_v`
- `current_latest_a`
- `current_min_a`
- `current_max_a`
- `current_avg_a`
- `ambient_temp_c`
- `pressure_hpa`

## Known Constraints

- `:tx` currently only creates `PROTOCOL_CUSTOM` / `BITS` messages.
- `:range` is an immediate ranging-request shortcut only; it does not expose
  cancellation, last-result state, or a dedicated range event stream.
- `:txat` only accepts `janus_type=sms` in the current implementation.
- `:cancel_tx` only cancels a request before the MAC host scheduler hands it to
  `MESS`.
- `:importcfg` currently mixes legacy CFG text output with the machine command
  response surface.
- `:config` is advertised but not implemented.
