# OpenAquatix ICD v2 (Host Automation Addendum)

## 1. Scope

This document defines minimal, backward-compatible additions to improve programmatic control of the OpenAquatix HMI over UART/USB.

## 2. v1 Wire Behavior (Observed in COMM)

## 2.1 Boot and Menu Rendering

At startup, HMI emits:
- `Welcome to the UAM HMI!\r\n`

Menu redraw format (`displaySubMenus()`):
1. Leading blank line: `\r\n`
2. Menu title line, e.g. `Main Menu\r\n`
3. Child items as numeric lines:
- `<N>: <description>\r\n`

Examples:
- `1: Configuration Menu`
- `4: Transmit and Receive Data Menu`

No canonical prompt token (such as `> `) is guaranteed in v1.

## 2.2 Navigation / Validation Output

Typical command feedback lines include:
- `\r\nInvalid option!\r\n`
- parameter prompts such as `Please enter a new value from ...:\r\n`
- toggle prompts from `COMMLoops_LoopToggle()`:
  - `\r\n<param> is currently enabled|disabled. Would you like to disable|enable it? (y/n):\r\n`
- TX/RX bits send prompt expects hex byte tokens (example `F6 1D`) and requires
  byte-count power-of-two input.
- TX/RX integer send prompt accepts unsigned range `0..4294967295`.

Important: many configurable actions are **toggle+confirm**, not deterministic set operations.

## 2.3 Received Message Printing (Exact v1 Pattern)

When enabled (`PARAM_PRINT_ENABLED=true`) and not dropped by error policy:
1. Header line:
- `\r\nReceived a new message at <seconds>s\r\n`
2. Optional cargo-error line:
- `Message cargo contained errors\r\n`
3. Protocol-specific fields, e.g.:
- `Errors Present: Yes|No\r\n`
- `Sender id: <n>\r\n`
- `Message Length (bits): <n>\r\n`
- `String: <payload>` or `Bits: ...` or `Integer: ...` or `Float: ...`
- JANUS variant fields (mobility, schedule, tx/rx, class id, etc.)
4. Trailing separator:
- `\r\n\r\n`

## 2.4 Asynchronous Interleaving

These lines can appear asynchronously relative to menu flow:
- `Dropped a packet with an invalid preamble\r\n`
- `Dropped a packet with an invalid cargo\r\n`

Also, typed input may be echoed/backspaced by the terminal echo path.

## 2.5 Deficiencies for Machine Use

- No stable line typing (menu vs prompt vs diagnostics vs RX data).
- No canonical prompt terminator.
- RX print block is multiline and can interleave with notifications.
- Toggle operations (not set operations) make deterministic host APIs difficult.
- Human-facing strings are used as protocol surface.

## 3. v2 Additions (Minimal + Backward-Compatible)

## 3.1 Typed Output Lines (`HMI_TAG`)

### Behavior
Optional mode (`:tag on`, `:tag off`) where each emitted line starts with one prefix token and one space:
- `[MENU]`
- `[PROMPT]`
- `[PARAM]`
- `[MSG_RX]`
- `[STATUS]`
- `[ERROR]`
- `[NOTIFY]`
- `[ECHO]`

### Notes
- Default remains `off` (v1 unchanged).
- In tag mode, every logical output line must have exactly one prefix.

## 3.2 Canonical Prompt Rule

All prompts must end with exact suffix `"> "`.

Examples:
- `[PROMPT] Select option > `
- `[PROMPT] Enter new value > `

This must apply to menu selection, parameter entry, and yes/no confirmation prompts.

## 3.3 Line Terminal Mode (`TXRX_LINE_MODE`)

### Why rename
v1 and proposed behavior are line-oriented text I/O, not arbitrary byte-stream framing. Therefore this mode is specified as **Line Terminal Mode** (not “raw binary mode”).

### Entry
New menu item under TX/RX:
- `Transmit/Receive -> Line Terminal Mode`

On entry:
- `[STATUS] LINE_MODE ENTERED`

### Data behavior
- Host line ending with CR/LF is transmitted as one payload line (CR/LF stripped).
- Incoming payload lines are emitted as:
  - `[MSG_RX] <payload>`
- Menu framing is suppressed while active.

### Exit
- Escape sequence at line start: `0x1D EXIT\r\n`
- Exit acknowledgment:
  - `[STATUS] LINE_MODE EXITED`
- Return to previous menu + canonical prompt.

### Literal escape handling
- Leading literal `0x1D` in payload is escaped as doubled `0x1D 0x1D`.

## 3.4 Deterministic Machine Commands

At prompt context, firmware now supports machine-oriented commands that do not
require menu driving:
- `:tag on|off`
- `:prompt on|off`
- `:print on|off`
- `:mode hostmac on|off`
- `:time`
- `:tx <base64>`
- `:txfb <base64>`
- `:range [route=transducer|feedback]`
- `:txat route=<transducer|feedback> protocol=<custom|janus> payload=<base64> tx_cyccnt=<u32> ...`
- `:cancel_tx <request_id>`
- `:rxsub on|off`
- `:sense on|off`
- `:status`
- `:telemetry [all|temp|power|electrical|env]`
- `:telemetrysub on|off [all|temp|power|electrical|env] [period_ms=<u32>]`
- `:errorlog`

` :txat ` required fields:
- `route`
- `protocol`
- `payload`
- `tx_cyccnt`
- `type=<custom_type>` when `protocol=custom`

` :txat ` optional preamble overrides:
- `modem_id=<u16>`
- `is_mobile=<0|1|true|false>`
- `reservation_time_10ms=<u16>`
- `schedule_flag=<u16>`
- `destination_id=<u16>`
- `tx_rx_capable=<0|1|true|false>`
- `can_forward=<0|1|true|false>`
- `coding=<u16>`
- `encryption=<u16>`
- `janus_type=sms`

Response contract:
- success: `[STATUS] OK <command> ...`
- failure: `[ERROR] <reason>`

Examples:
- `[STATUS] OK :time tick_ms=123456 cyccnt=987654321`
- `[STATUS] OK :mode hostmac on`
- `[STATUS] OK :range route=transducer`
- `[STATUS] OK :txat request_id=4 protocol=custom tx_cyccnt=123456789`
- `[STATUS] OK :status mode=hostmac rxsub=on sense=on telemetrysub=off telemetry_group=none telemetry_period_ms=0 ...`
- `[STATUS] OK :telemetry group=all tick_ms=123456 cyccnt=987654321 temp_ready=yes power_ready=yes electrical_ready=yes env_ready=yes ...`
- `[STATUS] OK :telemetrysub on group=power period_ms=1000`
- `[STATUS] OK :errorlog count=2 current_tick_ms=123456 current_reset_count=3`

## 3.5 Host Event Streams

When `:rxsub on` is enabled, decoded receive events are emitted as one tagged
line per packet:
- `[MSG_RX] EVENT protocol=<custom|janus> route=<transducer|feedback> timestamp_ms=<u32> rx_cyccnt=<u32> length_bits=<u16> error=<0|1> ... payload_b64=<base64>`

Included RX metadata may contain:
- `data_type=<custom_type>` for custom packets
- `janus_type=sms` for JANUS packets currently supported by firmware
- preamble-derived fields when valid:
  - `modem_id`
  - `is_mobile`
  - `reservation_time_10ms`
  - `schedule_flag`
  - `destination_id`
  - `tx_rx_capable`
  - `can_forward`
  - `coding`
  - `encryption`
- `snr`
- `doppler_mps`
- `range_m` for ranging responses

Dedicated ranging requests issued with `:range` reuse this same RX event path;
there is no separate ranging completion event.

When `:sense on` is enabled, channel-sensing telemetry is emitted as:
- `[NOTIFY] EVENT sense timestamp_ms=<u64> cyccnt=<u32> psd=<float>`

When `:telemetrysub on` is enabled, periodic telemetry is emitted as:
- `[NOTIFY] EVENT telemetry group=<all|temp|power|electrical|env> tick_ms=<u64> cyccnt=<u32> temp_ready=<yes|no> power_ready=<yes|no> electrical_ready=<yes|no> env_ready=<yes|no> ...`

## 4. Format Constraints for Testability

- One logical line per `\r\n`.
- No multiline prompts.
- RX payload must be emitted on `[MSG_RX]` lines only in tag mode.
- Async notifications must use `[NOTIFY]`.
- Menu redraw starts with a `[MENU]` title line, followed by `[MENU] <N>: ...` option lines.

## 5. Compatibility

- v1 default behavior remains valid when tag mode and line mode are unused.
- v2 features are strictly additive and opt-in.
- Host implementations should probe support (`:tag on`, `:help`) and fall back to v1 heuristics if unsupported.
