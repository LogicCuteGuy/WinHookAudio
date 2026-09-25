# One app uses the Master at a time

Date: 2026-09-25
Status: accepted

## Context
The Master DAW is the single DAW hosting the Master driver (CONTEXT.md). Nothing enforced it: a
second DAW choosing WinHookAudio Master mapped the same Slot Table, Master audio and Bridge memory,
ran a second Worker and Master Clock and opened the same HW devices, with the two fighting over all
of them. DAWs also create a second driver instance of their own while they scan drivers.

## Decision
- The first process whose Master `init()` runs claims the Master (`asio-master/MasterClaim.h`): a
  named file mapping `WinHookAudio_Master_Claim`, per Windows session like the Slot Table, holding
  its PID and exe name. Another process's `init()` returns ASIOFalse, and `getErrorMessage` says
  "WinHookAudio Master is in use by <exe> (PID n)", which DAWs show.
- Instances in the claiming process share the claim (counted); the last one to go releases it.
- A named mapping, not a mutex: a mutex belongs to a thread, and DAWs create and release drivers
  on different threads. A crashed owner's claim goes with its handles. If the owner exits while
  another app is reading its claim (which keeps the name alive for a moment), the reader retries.
- Bridges are not affected: any number of DAWs use Bridges next to the Master DAW.

## Consequences
- Live (`asio-live`, dev VM): while streaming, another process is refused with its name and PID; a
  second instance in the same process inits; after the owner lets go, or is killed, another
  process's init works.
- A test program that inits the Master while a DAW holds it is refused instead of writing into
  that DAW's Slot Table.
- Apps in different Windows sessions (another user signed in) are not stopped, the same as the
  Slot Table they would each have.
