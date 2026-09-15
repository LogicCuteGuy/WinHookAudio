# 04: Shared Bridge region

**What to build:** A verifiable Shared Bridge definition allowing up to four Slave DAWs per Bridge to share one channel set with summed input and broadcast output.

**Blocked by:** 01-slot-table-abi.

**Status:** ready-for-agent

- [ ] Client count enforces 0..4 and rejects a fifth Slave DAW with a clear reason
- [ ] Per-client ready and active-buffer flags plus mixed sum bank are defined
- [ ] Summing preserves loudness via soft-clip rather than silent averaging
- [ ] Offline checks pass with no audio hardware, DAW, or driver install
