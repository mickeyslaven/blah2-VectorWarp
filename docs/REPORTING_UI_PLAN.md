# Error-reporting UI plan

Implement this only after the current application work is merged into `main`.

## Placement

Use two entry points backed by the same review-and-consent dialog:

1. Add a compact **Report an issue** utility link at the bottom of Settings. It
   should appear after the configuration editor, not inside the sticky Save
   toolbar and not as a permanent warning-colored banner. Suggested copy:
   “Having trouble with VectorWarp? Send a diagnostic report.”
2. Add **Send report** to reportable error banners. Show it only for classified
   incidents such as SDR connection failures, processor errors, failed restarts
   and unsafe persistence outcomes. Expected validation and operator guidance
   should not show it.

Settings is the best permanent location because receiver setup and restart
failures already lead users there. A header-wide button would add visual weight
to every radar display and compete with live service status. If later user
feedback shows Settings is hard to reach during failures, add a small “Help”
menu to the global header rather than another primary navigation item.

## Consent dialog

Title: **Send diagnostic report?**

Body:

> VectorWarp will send diagnostic information to the project maintainer for
> analysis and bug fixing. This includes the error, VectorWarp and system
> versions, receiver type and operating details, service state, and a small
> sanitized diagnostic history.
>
> It will not include your location, network addresses, receiver serial
> numbers, full configuration, ADS-B data, recordings, raw IQ, or credentials.
>
> Select View report to inspect the exact data. If you have privacy concerns or
> want complete control over what is shared, cancel and file a GitHub issue
> instead.

Actions, in order: **View report**, **Cancel**, **Send report**. The GitHub issue
fallback should be a normal link and must not imply that GitHub is private.

## Behavior

- Never transmit before the user selects **Send report**.
- Require consent for every manual report.
- Show the exact final sanitized JSON in **View report**.
- Disable the send button while a submission is pending.
- On success, show the report ID. The private triage issue URL is never exposed
  to the browser.
- On failure, retain the report locally so the user can retry or copy it.
- Deduplicate prompts by stable incident fingerprint and recovery generation.
- A future automatic crash-report option requires a separate explicit opt-in.

## Reportable pathway contract

Each contextual action must build one of the following classified incidents.
The common report envelope always includes the VectorWarp version and commit,
OS/architecture/kernel, receiver type/model/driver/API and RF operating values,
processor/restart state, uptime, the current configuration revision hash, and a
bounded recent incident history. Pathway details are scalar, bounded values;
never include raw logs or configuration.

| Pathway | Stable incident code | Required pathway evidence |
| --- | --- | --- |
| Receiver discovery | `SDR_ENUMERATION_FAILED` | device count, permission result, driver loaded state, discovery stage |
| Receiver open | `SDR_OPEN_FAILED` | receiver family/model, driver/API versions, transport, driver error class |
| Receiver stream | `SDR_STREAM_LOST` | stream/callback state, frames received, last-frame age, channel count and sample rate |
| Receiver synchronization | `RECEIVER_SYNC_FAILED` | synchronization state, rollback and reconciliation states, configuration revision |
| Gain probe/monitor | `GAIN_MONITOR_FAILED` | probe timeout/result, attempt count, receiver family and RF operating values |
| Processor start | `PROCESSOR_START_FAILED` | exit status/signal, backend, receiver state and restart state |
| Processor runtime | `PROCESSOR_RUNTIME_FAILED` | processed frame count, last-frame age, queue depth and backend |
| GPU acceleration | `GPU_BACKEND_FAILED` | backend/driver state and CPU fallback attempt/result |
| Restart lifecycle | `RESTART_FAILED` | helper availability, receipt state, timeout and exit status |
| Configuration persistence | `CONFIG_PERSISTENCE_FAILED` | write stage, revision conflict and rollback attempt/result |
| API connection | `API_UNAVAILABLE` | logical route name, status/timeout and consecutive failure count; never the URL or address |
| ADS-B source | `ADSB_SOURCE_FAILED` | source and healthy-source counts, failure class and consecutive failures; never source endpoints or aircraft data |

Expected validation failures, unsupported optional features, setup guidance,
normal replay state and operator-correctable RF quality warnings do not receive
a contextual report action. The permanent Settings action may still produce a
manual report using the current status snapshot.
