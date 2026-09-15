# 06: Offline build integration

**What to build:** A green offline build proving the full ABI slice holds together with no audio claim.

**Blocked by:** 02-shm-names-sizes, 03-slots-json-schema, 04-shared-bridge-region, 05-whaa-packet-header.

**Status:** ready-for-agent

- [ ] Compile-time size and bound checks enforce the ABI invariants
- [ ] Command-line tests validate schema behavior without devices or network
- [ ] Existing offline CLI pattern keeps passing
- [ ] No audio functionality claimed without runtime verification
