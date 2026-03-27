# COMM Commands Test Plan

## Purpose

This plan verifies the command system implemented in
`Application/Src/COMM/comm_commands.c`, with emphasis on the host-control
commands added for Pi integration.

It covers:

- parser behavior
- success cases
- negative cases
- state transitions
- async RX/sense streams
- compatibility with the underlying firmware queues

## Test Environment

### Firmware

- build and flash the current firmware image
- use a build that includes the current `COMM`, `MAC`, `MESS`, and `CFG` changes

### Host Interfaces

Test both if available:

- USB CDC
- UART daughter-card path

### Equipment

- host terminal or automation harness that can send exact command lines
- optional second modem or loopback/acoustic setup for RX validation
- optional scripted harness for checking tagged output and timing

## General Preconditions

Before each command-specific test set:

1. Boot the modem and confirm the HMI is responsive.
2. Enable machine-readable output:
   - `:tag on`
3. Disable prompts unless explicitly testing them:
   - `:prompt off`
4. Record whether testing is over USB or UART.

## Parser and Framing Tests

### P1. Command Prefix Required

- Send a line without `:`
- Expected: command system does not claim it as a command

### P2. Leading Whitespace

- Send `  :status`
- Expected: command executes successfully

### P3. Case Insensitivity

- Send `:STATUS`
- Expected: same result as `:status`

### P4. Unknown Command

- Send `:doesnotexist`
- Expected: `[ERROR] Unknown command: :doesnotexist`

### P5. Quoted Argument Handling

- Send `:importcfg "START,...,END"`
- Expected: import parser receives the inner string correctly

### P6. Bad Quoting

- Send an unterminated quoted command argument
- Expected: usage error for the target command

## Core Session Commands

### T1. `:tag on|off`

- Toggle on, then off
- Expected:
  - `[STATUS] OK :tag on`
  - `[STATUS] OK :tag off`
- Verify subsequent command responses are still tagged because machine lines are forced-tagged

### T2. `:prompt on|off`

- Toggle on, then off
- Expected status responses
- With prompts enabled, navigate a menu and confirm prompt suffix is ` > `

### T3. `:print on|off`

- Toggle on, then off
- Expected status responses
- Confirm `PARAM_PRINT_ENABLED` behavior by receiving a packet with print on/off

## Host Mode Commands

### T4. `:mode hostmac on`

- Send `:mode hostmac on`
- Expected: `[STATUS] OK :mode hostmac on`
- Follow with `:status`
- Expected: `mode=hostmac`

### T5. `:mode hostmac off`

- Send `:mode hostmac off`
- Expected: `[STATUS] OK :mode hostmac off`
- Follow with `:status`
- Expected: `mode=local`

### T6. Invalid `:mode`

- Send `:mode hostmac maybe`
- Expected: usage error

## Time and Status Commands

### T7. `:time`

- Send `:time`
- Expected:
  - `[STATUS] OK :time tick_ms=<value> cyccnt=<value>`
- Validate:
  - `tick_ms` increases monotonically across repeated calls
  - `cyccnt` changes between calls

### T8. `:status`

- Send `:status`
- Expected one tagged status line containing:
  - `mode=...`
  - `rxsub=...`
  - `sense=...`
  - `mac_regular_tx_depth=...`
  - `mac_emergency_tx_depth=...`
  - `mess_tx_depth=...`
  - `pending_scheduled_tx=...`
  - `request_id=...`
  - `tx_cyccnt=...`

## Immediate TX Command

### T9. `:tx` Through Transducer

- Send:
  - `:tx route=transducer payload=QQ==`
- Expected:
  - `[STATUS] OK :tx route=transducer`
- Verify queue depth or physical TX activity if observable

### T10. `:tx` Through Feedback

- Send:
  - `:tx route=feedback payload=QQ==`
- Expected:
  - `[STATUS] OK :tx route=feedback`

### T11. `:tx` Missing Route

- Send:
  - `:tx payload=QQ==`
- Expected: usage error

### T12. `:tx` Missing Payload

- Send:
  - `:tx route=transducer`
- Expected: usage error

### T13. `:tx` Invalid Route

- Send:
  - `:tx route=invalid payload=QQ==`
- Expected: `[ERROR] Invalid route: invalid`

### T14. `:tx` Invalid Base64

- Send:
  - `:tx route=transducer payload=%%%`
- Expected: queue failure / decode failure response

### T15. `:tx` Oversized Payload

- Send a base64 payload that decodes past `PACKET_DATA_MAX_LENGTH_BYTES`
- Expected: queue failure / decode failure response

## Scheduled TX Command

### T16. Happy Path Custom Scheduled TX

1. `:mode hostmac on`
2. `:time`
3. pick a future `tx_cyccnt`
4. send:
   - `:txat route=transducer protocol=custom payload=QQ== tx_cyccnt=<future> type=bits`

Expected:

- `[STATUS] OK :txat request_id=<id> protocol=custom tx_cyccnt=<future>`
- `:status` shows `pending_scheduled_tx=yes`

### T17. Scheduled TX Feedback Route

- Same as T16 but with `route=feedback`
- Expected: success

### T18. Scheduled JANUS SMS

- Send:
  - `:txat route=transducer protocol=janus payload=QQ== tx_cyccnt=<future> janus_type=sms`
- Expected: success

### T19. Missing Required `txat` Fields

Verify usage/error behavior for:

- missing `route`
- missing `protocol`
- missing `payload`
- missing `tx_cyccnt`
- missing `type` for `protocol=custom`

### T20. Host Mode Required

- Ensure local mode
- Send a valid `:txat`
- Expected:
  - `[ERROR] Host MAC mode must be enabled before using :txat`

### T21. Bad `tx_cyccnt`

- non-numeric
- too close to current time
- in the past
- beyond safe wrap window

Expected:

- command rejects scheduling

### T22. Bad Optional Fields

Verify error handling for invalid:

- `janus_type`
- `is_mobile`
- `tx_rx_capable`
- `can_forward`
- `coding`
- `encryption`
- unknown extra option

### T23. One Pending Scheduled TX Limit

1. Queue one valid `:txat`
2. Before it is handed off, queue a second valid `:txat`

Expected:

- second command fails because one scheduled TX is already pending

## Cancel Scheduled TX

### T24. Cancel Pending Request

1. Queue valid `:txat`
2. Send `:cancel_tx <request_id>`

Expected:

- `[STATUS] OK :cancel_tx request_id=<id>`
- `:status` shows `pending_scheduled_tx=no`

### T25. Cancel Unknown Request

- Send `:cancel_tx 999999`
- Expected:
  - `[ERROR] No pending scheduled transmission with id=999999`

### T26. Cancel After Handoff

1. Queue scheduled TX far enough ahead to be accepted
2. Wait until firmware likely handed it to `MESS`
3. Send `:cancel_tx <request_id>`

Expected:

- cancel fails
- document the observed failure text

## RX and Sensing Subscriptions

### T27. `:rxsub on|off`

- Toggle on then off
- Expected status lines

### T28. RX Event Emission

1. `:rxsub on`
2. cause a valid inbound packet

Expected:

- one `[MSG_RX] EVENT ...` line per received packet
- validate presence of:
  - `protocol`
  - `route`
  - `timestamp_ms`
  - `rx_cyccnt`
  - `length_bits`
  - `error`
  - `payload_b64` when payload exists

### T29. `:sense on|off`

- Toggle on then off
- Expected status lines

### T30. Sense Event Emission

1. `:mode hostmac on`
2. `:sense on`

Expected:

- repeated `[NOTIFY] EVENT sense timestamp_ms=<u64> cyccnt=<u32> psd=<float>`

### T31. Interface Scoping

- Enable `:rxsub on` on USB only
- Generate RX
- Expected: stream appears only on USB session

## Import Configuration Command

### T32. Happy Path `:importcfg`

1. Generate or capture a valid exported blob:
   - `START,<id>-<value>,...,END`
2. Send:
   - `:importcfg <blob>`

Expected:

- legacy success text:
  - `Successfully imported <N> parameters`
- machine completion:
  - `[STATUS] OK :importcfg`

### T33. Missing START

- Send malformed blob without `START`
- Expected:
  - `Error: start sequence not found`

### T34. Missing END

- Send malformed blob without `END`
- Expected:
  - `Error: end sequence not found`

### T35. Invalid Parameter ID

- Send blob with out-of-range ID
- Expected:
  - `Invalid id received!`

### T36. Invalid Field Format

- Send blob with missing `-` separator
- Expected:
  - `Invalid format!`

### T37. Wrong Parameter Count

- Send blob with too few valid entries
- Expected:
  - `Error: Imported <N> parameters while <expected> expected`

### T38. Range Failure

- Send blob with a value outside a parameter’s allowed range
- Expected:
  - `Failed to set parameter with ID <id>`

## Stub Command

### T39. `:config`

- Send `:config`
- Expected:
  - `[ERROR] Config command not implemented yet`

## Regression Checks

### T40. Menu Navigation Still Works

- Navigate menus normally after using several commands
- Expected: menu system remains responsive

### T41. Existing RX Printing Still Works

- With `:print on`, receive a packet
- Expected: legacy multi-line print path still works

### T42. Command/Async Interleaving

- Enable `:rxsub on` and `:sense on`
- Issue commands while RX or sense events are active
- Expected:
  - command responses remain parseable
  - event lines remain single-line and tagged

## Exit Criteria

The command system passes when:

- all success-path commands behave as documented
- all negative tests fail deterministically
- no command crashes or wedges the COMM task
- scheduled TX can be queued, observed in status, and canceled before handoff
- RX and sense events stream in the documented format
- `importcfg` accepts the same blob used by the menu importer
