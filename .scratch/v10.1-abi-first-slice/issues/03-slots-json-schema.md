# 03: Persistent slots schema

**What to build:** An offline persistent schema round-trip preserving Slot indices, names, enabled state, opt-in Loopback, global settings, and Network Streams across save and load.

**Blocked by:** 01-slot-table-abi.

**Status:** ready-for-agent

- [ ] Save then load restores all 512 input and 512 output positions without shifting
- [ ] Overlong names, out-of-range counts, and Loopback on non-virtual Slots are rejected
- [ ] Version increments on Save to trigger Master DAW re-query
- [ ] Offline checks pass with no audio hardware, DAW, or admin rights
