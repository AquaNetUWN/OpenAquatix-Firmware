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
  - `telemetrysub=...`
  - `telemetry_group=...`
  - `telemetry_period_ms=...`

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

## Dedicated Ranging Command

### T15A. `:range` Default Route

- Send:
  - `:range`
- Expected:
  - `[STATUS] OK :range route=transducer`

### T15B. `:range` Feedback Route

- Send:
  - `:range route=feedback`
- Expected:
  - `[STATUS] OK :range route=feedback`

### T15C. `:range` Explicit Transducer Route

- Send:
  - `:range route=transducer`
- Expected:
  - `[STATUS] OK :range route=transducer`

### T15D. `:range` Invalid Arguments

- Send each of:
  - `:range route=invalid`
  - `:range foo`
  - `:range route=transducer extra`
  - `:range route=`
- Expected:
  - usage error for each
  - no ranging request is queued

### T15E. `:range` Works In Local Mode

1. Send `:mode hostmac off`
2. Send `:range`

Expected:

- `[STATUS] OK :mode hostmac off`
- `[STATUS] OK :range route=transducer`
- no host-mode rejection

### T15F. `:range` Result Reuse

1. Enable `:rxsub on`
2. Send `:range`
3. Complete a ranging exchange

Expected:

- normal `[MSG_RX] EVENT ... range_m=<float> ...` output for the ranging
  response
- no new dedicated ranging event line

Also verify with `:print on` that the legacy human-readable range output still
appears for the same response.

### T15G. `:range` Failure Reuse And Regression

- Induce a ranging request failure case after sending `:range`
- Expected:
  - existing async notify text still appears, such as `Failed to send ranging request`
  - no new dedicated ranging notify line appears
- Regression checks:
  - existing `:txat ... type=ranging_request` behavior remains unchanged
  - existing TX/RX menu ranging entries still work

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

## Telemetry Commands

### T40. `:telemetry` Default Snapshot

- Send `:telemetry`
- Expected one tagged status line containing:
  - `group=all`
  - `tick_ms=...`
  - `cyccnt=...`
  - `temp_ready=...`
  - `power_ready=...`
  - `electrical_ready=...`
  - `env_ready=...`

### T41. `:telemetry` Grouped Snapshots

- Send each of:
  - `:telemetry temp`
  - `:telemetry power`
  - `:telemetry electrical`
  - `:telemetry env`
- Expected:
  - one tagged status line per command
  - `group=<requested group>`
  - only the requested group’s numeric fields are present
  - all four ready flags are still present

### T42. Early-Boot Readiness

- Issue `:telemetry` immediately after boot or immediately after a task reset
- Expected:
  - groups without valid samples report `*_ready=no`
  - numeric fields for those groups are omitted rather than zero-filled

### T43. `:telemetrysub on` Defaults

- Send `:telemetrysub on`
- Expected:
  - `[STATUS] OK :telemetrysub on group=all period_ms=1000`
  - `:status` shows `telemetrysub=on telemetry_group=all telemetry_period_ms=1000`

### T44. `:telemetrysub` Reconfigure

- Send `:telemetrysub on power period_ms=250`
- Expected:
  - `[STATUS] OK :telemetrysub on group=power period_ms=250`
  - `:status` shows `telemetry_group=power`
  - `:status` shows `telemetry_period_ms=250`

### T45. Telemetry Event Emission

1. Send `:telemetrysub on env period_ms=1000`
2. Wait for at least two timer periods

Expected:

- repeated `[NOTIFY] EVENT telemetry ...` lines
- each line contains:
  - `group=env`
  - `tick_ms`
  - `cyccnt`
  - all four ready flags
- when `env_ready=yes`, the line includes:
  - `ambient_temp_c`
  - `pressure_hpa`

### T46. `:telemetrysub off`

- Send `:telemetrysub off`
- Expected:
  - `[STATUS] OK :telemetrysub off`
  - `:status` shows `telemetrysub=off telemetry_group=none telemetry_period_ms=0`
  - telemetry events stop

### T47. Invalid Telemetry Subscription Arguments

- Verify usage/error behavior for:
  - `:telemetrysub`
  - `:telemetrysub on invalid`
  - `:telemetrysub on power period_ms=50`
  - `:telemetrysub on power period_ms=70000`
  - `:telemetrysub off power`
- Expected:
  - usage error
  - active telemetry stream state does not change

## Error Log Command

### T48. Empty `:errorlog`

- Clear or start from an empty error log if possible
- Send `:errorlog`
- Expected:
  - `[STATUS] OK :errorlog count=0 current_tick_ms=<value> current_reset_count=<value>`
  - no entry lines follow

### T49. Populated `:errorlog`

- Trigger or retain at least two known errors
- Send `:errorlog`
- Expected:
  - summary line with `count>=1`
  - one `[STATUS] ENTRY :errorlog ...` line per retained entry
  - each entry contains:
    - `index`
    - `timestamp_ms`
    - `reset_count`
    - `error_code`
    - `severity`
    - `file`
    - `task`
    - `line`
    - `occurrences`
    - `description_b64`

### T50. Error Log Ordering And Encoding

- For a populated log:
  - confirm entries are ordered by `reset_count`, then `timestamp_ms`
  - decode `description_b64`
- Expected:
  - decoded text matches the current human-readable error description
  - no spaces appear in the `severity` token

## Regression Checks

### T51. Menu Navigation Still Works

- Navigate menus normally after using several commands
- Expected: menu system remains responsive

### T52. Existing RX Printing Still Works

- With `:print on`, receive a packet
- Expected: legacy multi-line print path still works

### T53. Command/Async Interleaving

- Enable `:rxsub on`, `:sense on`, and `:telemetrysub on`
- Issue commands while RX, sense, or telemetry events are active
- Expected:
  - command responses remain parseable
  - event lines remain single-line and tagged

## Exit Criteria

The command system passes when:

- all success-path commands behave as documented
- all negative tests fail deterministically
- no command crashes or wedges the COMM task
- scheduled TX can be queued, observed in status, and canceled before handoff
- RX, sense, and telemetry events stream in the documented format
- `importcfg` accepts the same blob used by the menu importer
