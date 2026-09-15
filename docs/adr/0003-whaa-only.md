# WHAA only, no VBAN compatibility

v10.1 uses WHAA My Version Only with PCM_F32/PCM_I16/Vorbis over UDP 6980/6981, because per-stream codec choice and Vorbis codebook handshake are required for 512-channel WAN use and VBAN framing would not carry them.
