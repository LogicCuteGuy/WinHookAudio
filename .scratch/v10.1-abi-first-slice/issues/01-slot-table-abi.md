# 01: Slot Table ABI

**What to build:** A verifiable Slot Table definition covering 512 input Slots and 512 output Slots with independent counts, stable indices, custom names, Master Clock plus per-thing Worker FIFOs, and Network Streams.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [ ] Input and output counts validate 1..512 and empty Slots count but stay silent
- [ ] Slot names truncate safely at 32 characters and map to Master DAW labels
- [ ] Loopback flag is meaningful only on Virtual Cable Slots and defaults off
- [ ] Bridge types live inside the pool, not as extra pool
- [ ] Offline checks pass with no audio hardware, DAW, driver install, or network
