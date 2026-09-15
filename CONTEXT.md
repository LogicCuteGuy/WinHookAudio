# WinHookAudio

WinHookAudio routes all PC audio through a Master DAW acting as mixer and effects rack.

## Language

**Master DAW**:
The single DAW hosting the Master driver that mixes and routes all audio.
_Avoid_: Main DAW, host DAW

**Slave DAW**:
A DAW hosting a Bridge driver that sends and receives a subset of channels.
_Avoid_: Client DAW, secondary DAW

**Master Driver**:
The ASIO driver exposing the full channel pool to the Master DAW.
_Avoid_: Primary driver

**Bridge Driver**:
An ASIO driver exposing a subset of the pool to one Slave DAW.
_Avoid_: Slave driver, client driver

**Virtual Cable**:
A Windows playback and recording endpoint pair owned by WinHookAudio.
_Avoid_: Virtual device, cable

**Slot**:
One named input or output position in the Master pool with a fixed index.
_Avoid_: Channel, cell

**Slot Table**:
The versioned map of all input and output slots, network streams, and global settings.
_Avoid_: Channel map, routing table

**Master Clock**:
The single sample rate and buffer size driving all data movement.
_Avoid_: DAW clock, engine clock

**Loopback**:
A Worker copy from a virtual output slot to its paired virtual input slot.
_Avoid_: Passthrough, monitor tap

**Shared Bridge**:
One Bridge channel set summed across up to four Slave DAWs.
_Avoid_: Shared client, mixed bridge

**Network Stream**:
One addressed WHAA send or receive flow carrying one or more slot channels.
_Avoid_: LAN channel, VBAN stream

**Control Panel**:
The native configuration UI opened from the driver's controlPanel entry.
_Avoid_: Settings window, popup
