/* ARM64X forwarder (ADR 0016): the DLL has no code of its own, only exports that forward to the
   ARM64 DLL (ARM64 processes) or the x64 DLL (x64 processes, emulated). The linker needs an object
   for each of its two halves; this one is built twice (ARM64 and ARM64EC). */
int wha_arm64x_forwarder_placeholder;
