# 02: Shared-memory names and sizes

**What to build:** A validated naming and sizing set for the Slot Table, Master audio ping-pong, four Shared Bridge regions, Master tick, Bridge ticks, and TableChanged signals.

**Blocked by:** 01-slot-table-abi.

**Status:** ready-for-agent

- [ ] All names are unique, stable, and documented for Master Driver creation versus Bridge Driver open
- [ ] Control-plane size class and data-plane sizes are fixed and verified offline
- [ ] Closing the Master DAW implies silence with no stale replay
- [ ] Offline checks pass with no audio hardware, DAW, or driver install
