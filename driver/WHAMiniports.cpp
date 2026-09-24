// WHAMiniports - the wave (WaveRT) and topology miniports of one Virtual Cable side, and the WaveRT
// stream that copies between the Windows audio engine's cyclic buffer and the cable's ring.
// Vocabulary: Virtual Cable, Worker.

#include "WHADriver.h"

#include "../common/virtual/WHACableFormat.h"

namespace wha {
namespace {

// ---------------------------------------------------------------------------------------------------
// Pin and filter descriptions

// Each cable's endpoint names: HKLM\...\MediaCategories\{this}\Name, written by WinHookAudio.inf.
// Playback {5EDB54C0-...} = "WinHookAudio Output 1" ... {5EDB54C7-...} = "WinHookAudio Output 8";
// recording {5EDB54D0-...} = "WinHookAudio Input 1" ... {5EDB54D7-...} = "WinHookAudio Input 8".
#define WHA_CABLE_NAME(first, n) {first + n, 0x7282, 0x42f3, {0x9c, 0x57, 0xf4, 0x83, 0xae, 0x5a, 0x32, 0xac}}
#define WHA_CABLE_NAMES(first)                                                                              \
  {WHA_CABLE_NAME(first, 0), WHA_CABLE_NAME(first, 1), WHA_CABLE_NAME(first, 2), WHA_CABLE_NAME(first, 3), \
   WHA_CABLE_NAME(first, 4), WHA_CABLE_NAME(first, 5), WHA_CABLE_NAME(first, 6), WHA_CABLE_NAME(first, 7)}
const GUID kOutputNames[kCables] = WHA_CABLE_NAMES(0x5edb54c0);
const GUID kInputNames[kCables] = WHA_CABLE_NAMES(0x5edb54d0);
#undef WHA_CABLE_NAMES
#undef WHA_CABLE_NAME

KSDATARANGE g_bridgeRange = {
    sizeof(KSDATARANGE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)};
PKSDATARANGE g_bridgeRanges[] = {&g_bridgeRange};

#define WHA_PIN(maxInstances, ranges, flow, communication, category, name)                                \
  {maxInstances, maxInstances, 0, nullptr,                                                               \
   {0, nullptr, 0, nullptr, SIZEOF_ARRAY(ranges), ranges, flow, communication, category, name, 0}}
// A wave filter's streaming pin: its data range and automation are the cable's (WHAWaveMiniport).
#define WHA_STREAM_PIN(flow) \
  {1, 1, 0, nullptr, {0, nullptr, 0, nullptr, 0, nullptr, flow, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO, nullptr, 0}}

const PCPIN_DESCRIPTOR g_waveRenderPins[] = {
    WHA_STREAM_PIN(KSPIN_DATAFLOW_IN),
    WHA_PIN(0, g_bridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr),
};
const PCPIN_DESCRIPTOR g_waveCapturePins[] = {
    WHA_PIN(0, g_bridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr),
    WHA_STREAM_PIN(KSPIN_DATAFLOW_OUT),
};
const PCPIN_DESCRIPTOR g_topoRenderPins[] = {
    WHA_PIN(0, g_bridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr),
    // Line, not speaker: as a speaker Windows showed every one of these endpoints as "Speakers"
    // although the pin is named (with or without a jack description); VB-Cable's line endpoint keeps its name.
    WHA_PIN(0, g_bridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_LINE_CONNECTOR, nullptr),  // name: per cable
};
const PCPIN_DESCRIPTOR g_topoCapturePins[] = {
    WHA_PIN(0, g_bridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_MICROPHONE, nullptr),  // name: per cable
    WHA_PIN(0, g_bridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr),
};
static_assert(kWaveRenderStreamPin == 0 && kWaveRenderBridgePin == 1, "g_waveRenderPins order");
static_assert(kWaveCaptureBridgePin == 0 && kWaveCaptureStreamPin == 1, "g_waveCapturePins order");
static_assert(kTopoRenderWavePin == 0 && kTopoRenderSpeakerPin == 1, "g_topoRenderPins order");
static_assert(kTopoCaptureMicPin == 0 && kTopoCaptureWavePin == 1, "g_topoCapturePins order");

// A wave filter passes its input pin straight to its output pin.
const PCCONNECTION_DESCRIPTOR g_passThrough[] = {{PCFILTER_NODE, 0, PCFILTER_NODE, 1}};

// A topology filter: input pin -> volume node -> mute node -> output pin. Windows' volume slider and
// mute button drive the nodes; the stream applies them (WHACableLevel).
constexpr ULONG kVolumeNode = 0, kMuteNode = 1;
constexpr LONG kVolumeMin = -96 * 65536, kVolumeMax = 0, kVolumeStep = 65536 / 2;  // 1/65536 dB: -96..0 by 0.5
NTSTATUS TopologyNodeProperty(PPCPROPERTY_REQUEST request);

const PCPROPERTY_ITEM g_volumeProperties[] = {
    {&KSPROPSETID_Audio, KSPROPERTY_AUDIO_VOLUMELEVEL,
     KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, TopologyNodeProperty},
};
const PCPROPERTY_ITEM g_muteProperties[] = {
    {&KSPROPSETID_Audio, KSPROPERTY_AUDIO_MUTE, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
     TopologyNodeProperty},
};
DEFINE_PCAUTOMATION_TABLE_PROP(g_volumeAutomation, g_volumeProperties);
DEFINE_PCAUTOMATION_TABLE_PROP(g_muteAutomation, g_muteProperties);
const PCNODE_DESCRIPTOR g_topoRenderNodes[] = {
    {0, &g_volumeAutomation, &KSNODETYPE_VOLUME, &KSAUDFNAME_MASTER_VOLUME},
    {0, &g_muteAutomation, &KSNODETYPE_MUTE, &KSAUDFNAME_MASTER_MUTE},
};
const PCNODE_DESCRIPTOR g_topoCaptureNodes[] = {
    {0, &g_volumeAutomation, &KSNODETYPE_VOLUME, &KSAUDFNAME_MIC_VOLUME},
    {0, &g_muteAutomation, &KSNODETYPE_MUTE, &KSAUDFNAME_MIC_MUTE},
};
const PCCONNECTION_DESCRIPTOR g_topoConnections[] = {
    {PCFILTER_NODE, 0, kVolumeNode, KSNODEPIN_STANDARD_IN},
    {kVolumeNode, KSNODEPIN_STANDARD_OUT, kMuteNode, KSNODEPIN_STANDARD_IN},
    {kMuteNode, KSNODEPIN_STANDARD_OUT, PCFILTER_NODE, 1},
};

// No jack description (KSPROPERTY_JACK_DESCRIPTION): Windows builds the endpoints without one, as for
// VB-Cable, which answers none.

// Wave filters list both directions, as PortCls' defaults (and VB-Cable's filters) do.
const GUID g_waveCategories[] = {STATICGUIDOF(KSCATEGORY_AUDIO), STATICGUIDOF(KSCATEGORY_RENDER),
                                 STATICGUIDOF(KSCATEGORY_CAPTURE), STATICGUIDOF(KSCATEGORY_REALTIME)};
const GUID g_topoCategories[] = {STATICGUIDOF(KSCATEGORY_AUDIO), STATICGUIDOF(KSCATEGORY_TOPOLOGY)};

// A Windows stream's format.
struct StreamFormat {
  ULONG rate;
  ULONG channels;
  WHASampleKind kind;
};

// A format the stream can convert: 32-bit float, 16/24/32-bit PCM or 24 bits in 32, 1..8 channels (more
// than 2 only as EXTENSIBLE), WAVEFORMATEX or EXTENSIBLE. Whether the cable offers it: Offers().
bool ParseFormat(const KSDATAFORMAT* format, StreamFormat* out) {
  if (!format || format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) return false;
  if (!IsEqualGUIDAligned(format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
      !IsEqualGUIDAligned(format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    return false;
  const bool subFloat = IsEqualGUIDAligned(format->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
  const bool subPcm = IsEqualGUIDAligned(format->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
  const WAVEFORMATEX& w = reinterpret_cast<const KSDATAFORMAT_WAVEFORMATEX*>(format)->WaveFormatEx;
  bool isFloat = w.wFormatTag == WAVE_FORMAT_IEEE_FLOAT, isPcm = w.wFormatTag == WAVE_FORMAT_PCM;
  ULONG valid = w.wBitsPerSample;
  if (w.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    if (w.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) ||
        format->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE))
      return false;
    const auto& x = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(w);
    valid = x.Samples.wValidBitsPerSample;
    isFloat = IsEqualGUIDAligned(x.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    isPcm = IsEqualGUIDAligned(x.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
  } else if (w.nChannels > 2) {
    return false;
  }
  const ULONG bits = w.wBitsPerSample;
  WHASampleKind kind;
  if (isFloat && subFloat && bits == 32 && valid == 32) kind = WHASampleKind::Float32;
  else if (isPcm && subPcm && bits == 16 && valid == 16) kind = WHASampleKind::Pcm16;
  else if (isPcm && subPcm && bits == 24 && valid == 24) kind = WHASampleKind::Pcm24;
  else if (isPcm && subPcm && bits == 32 && valid == 32) kind = WHASampleKind::Pcm32;
  else if (isPcm && subPcm && bits == 32 && valid == 24) kind = WHASampleKind::Pcm24in32;
  else return false;
  if (w.nChannels < 1 || w.nChannels > kCableChannels || w.nBlockAlign != FrameBytes(kind, w.nChannels) || !w.nSamplesPerSec)
    return false;
  out->rate = w.nSamplesPerSec;
  out->channels = w.nChannels;
  out->kind = kind;
  return true;
}

// A cable offers exactly one format (WHACable::format): the Master's rate, the cable's channels and
// sample format. Windows gets no other, so what the panel sets is what Windows streams.
bool Offers(const WHACableFormat& cable, const StreamFormat& f) {
  return f.rate == cable.rate && f.channels == cable.channels && f.kind == cable.kind;
}

WHACableFormat CurrentFormat(WHACable& c) {
  KIRQL irql;
  KeAcquireSpinLock(&c.lock, &irql);
  const WHACableFormat format = c.format;
  KeReleaseSpinLock(&c.lock, irql);
  return format;
}

// The cable of the wave filter a property or event request is for (its MajorTarget).
WHACable& CableOf(PUNKNOWN majorTarget);

// KSPROPERTY_PIN_PROPOSEDATAFORMAT on a wave filter's streaming pin: the audio engine asks it to pick
// the device format (the endpoint stays "not present" without an answer). PortCls does not answer it
// from the data ranges.
NTSTATUS ProposeFormat(PPCPROPERTY_REQUEST request, ULONG streamPin) {
  if (request->InstanceSize < sizeof(ULONG)) return STATUS_INVALID_PARAMETER;
  const ULONG pin = *static_cast<const ULONG*>(request->Instance);
  if (request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
    if (request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
      auto* d = static_cast<PKSPROPERTY_DESCRIPTION>(request->Value);
      d->AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_SET;
      d->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
      d->PropTypeSet.Set = KSPROPTYPESETID_General;
      d->PropTypeSet.Id = VT_ILLEGAL;
      d->PropTypeSet.Flags = 0;
      d->MembersListCount = 0;
      d->Reserved = 0;
      request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
    } else if (request->ValueSize >= sizeof(ULONG)) {
      *static_cast<PULONG>(request->Value) = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_SET;
      request->ValueSize = sizeof(ULONG);
    } else {
      return STATUS_BUFFER_TOO_SMALL;
    }
    return STATUS_SUCCESS;
  }
  if (!(request->Verb & KSPROPERTY_TYPE_SET)) return STATUS_INVALID_DEVICE_REQUEST;
  if (pin != streamPin) return STATUS_NO_MATCH;
  if (request->ValueSize < sizeof(KSDATAFORMAT)) return STATUS_BUFFER_TOO_SMALL;
  const auto* format = static_cast<const KSDATAFORMAT*>(request->Value);
  if (format->FormatSize > request->ValueSize) return STATUS_BUFFER_TOO_SMALL;
  if (format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
    // Only the header (the endpoint builder asks this way): is this kind of format supported at all?
    const auto matches = [](const GUID& asked, const GUID& ours, const GUID& wildcard) {
      return IsEqualGUIDAligned(asked, ours) || IsEqualGUIDAligned(asked, wildcard);
    };
    const bool audio = matches(format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO, KSDATAFORMAT_TYPE_WILDCARD);
    const bool sub = matches(format->SubFormat, KSDATAFORMAT_SUBTYPE_PCM, KSDATAFORMAT_SUBTYPE_WILDCARD) ||
                     IsEqualGUIDAligned(format->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    const bool specifier = matches(format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX, KSDATAFORMAT_SPECIFIER_WILDCARD);
    return audio && sub && specifier ? STATUS_SUCCESS : STATUS_NO_MATCH;
  }
  StreamFormat parsed;
  return ParseFormat(format, &parsed) && Offers(CurrentFormat(CableOf(request->MajorTarget)), parsed) ? STATUS_SUCCESS
                                                                                                     : STATUS_NO_MATCH;
}
NTSTATUS ProposeRenderFormat(PPCPROPERTY_REQUEST request) { return ProposeFormat(request, kWaveRenderStreamPin); }
NTSTATUS ProposeCaptureFormat(PPCPROPERTY_REQUEST request) { return ProposeFormat(request, kWaveCaptureStreamPin); }

// `f` as the WAVEFORMATEXTENSIBLE of the cable's format `cf`.
void FillFormat(KSDATAFORMAT_WAVEFORMATEXTENSIBLE* f, const WHACableFormat& cf) {
  const ULONG frameBytes = FrameBytes(cf.kind, cf.channels);
  const GUID& subFormat = cf.kind == WHASampleKind::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
  RtlZeroMemory(f, sizeof(*f));
  f->DataFormat.FormatSize = sizeof(*f);
  f->DataFormat.SampleSize = frameBytes;
  f->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
  f->DataFormat.SubFormat = subFormat;
  f->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
  WAVEFORMATEXTENSIBLE& w = f->WaveFormatExt;
  w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  w.Format.nChannels = static_cast<WORD>(cf.channels);
  w.Format.nSamplesPerSec = cf.rate;
  w.Format.wBitsPerSample = static_cast<WORD>(SampleBytes(cf.kind) * 8);
  w.Format.nBlockAlign = static_cast<WORD>(frameBytes);
  w.Format.nAvgBytesPerSec = cf.rate * frameBytes;
  w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  w.Samples.wValidBitsPerSample = static_cast<WORD>(ValidBits(cf.kind));
  w.dwChannelMask = CableChannelMask(cf.channels);
  w.SubFormat = subFormat;
}

// KSPROPERTY_PIN_PROPOSEDATAFORMAT2 on a wave filter's streaming pin: the device's default format for
// a signal processing mode (the instance's attribute list). We have no modes, so the default and raw
// modes share the cable's one format. The reply is the format flagged with attributes, then the
// attribute list back, 8-byte aligned.
NTSTATUS ProposeFormat2(PPCPROPERTY_REQUEST request, ULONG streamPin) {
  constexpr ULONG kPinFields = sizeof(KSP_PIN) - sizeof(KSPROPERTY);  // PinId, Reserved
  if (request->InstanceSize < kPinFields) return STATUS_INVALID_PARAMETER;
  if (*static_cast<const ULONG*>(request->Instance) != streamPin) return STATUS_NOT_SUPPORTED;
  if (request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
    if (request->ValueSize < sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
    *static_cast<PULONG>(request->Value) = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET;
    request->ValueSize = sizeof(ULONG);
    return STATUS_SUCCESS;
  }
  if (!(request->Verb & KSPROPERTY_TYPE_GET)) return STATUS_INVALID_DEVICE_REQUEST;
  const auto* attributes = reinterpret_cast<const KSMULTIPLE_ITEM*>(static_cast<const BYTE*>(request->Instance) + kPinFields);
  const ULONG attributesSize = request->InstanceSize - kPinFields;
  if (attributesSize) {
    if (attributesSize < sizeof(KSMULTIPLE_ITEM) || attributes->Size < sizeof(KSMULTIPLE_ITEM) ||
        attributes->Size > attributesSize)
      return STATUS_INVALID_PARAMETER;
    ULONG left = attributes->Size - sizeof(KSMULTIPLE_ITEM);
    const auto* a = reinterpret_cast<const KSATTRIBUTE*>(attributes + 1);
    for (ULONG i = 0; i < attributes->Count; ++i) {
      if (left < sizeof(KSATTRIBUTE) || a->Size < sizeof(KSATTRIBUTE) || a->Size > left) return STATUS_INVALID_PARAMETER;
      if (!IsEqualGUIDAligned(a->Attribute, KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE) ||
          a->Size != sizeof(KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE))
        return STATUS_NOT_SUPPORTED;
      const GUID& mode = reinterpret_cast<const KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE*>(a)->SignalProcessingMode;
      if (!IsEqualGUIDAligned(mode, AUDIO_SIGNALPROCESSINGMODE_DEFAULT) &&
          !IsEqualGUIDAligned(mode, AUDIO_SIGNALPROCESSINGMODE_RAW))
        return STATUS_NOT_SUPPORTED;
      const ULONG step = (a->Size + FILE_QUAD_ALIGNMENT) & ~ULONG(FILE_QUAD_ALIGNMENT);
      if (step >= left) break;
      left -= step;
      a = reinterpret_cast<const KSATTRIBUTE*>(reinterpret_cast<const BYTE*>(a) + step);
    }
  }
  const ULONG formatSize = (sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) + 7) & ~7UL;
  const ULONG size = formatSize + (attributesSize ? attributes->Size : 0);
  if (!request->ValueSize) {
    request->ValueSize = size;
    return STATUS_BUFFER_OVERFLOW;
  }
  if (request->ValueSize < size) return STATUS_BUFFER_TOO_SMALL;
  auto* format = static_cast<KSDATAFORMAT_WAVEFORMATEXTENSIBLE*>(request->Value);
  RtlZeroMemory(format, formatSize);
  FillFormat(format, CurrentFormat(CableOf(request->MajorTarget)));
  if (attributesSize) {
    format->DataFormat.Flags = KSDATAFORMAT_ATTRIBUTES;
    RtlCopyMemory(static_cast<BYTE*>(request->Value) + formatSize, attributes, attributes->Size);
  }
  request->ValueSize = size;
  return STATUS_SUCCESS;
}
NTSTATUS ProposeRenderFormat2(PPCPROPERTY_REQUEST request) { return ProposeFormat2(request, kWaveRenderStreamPin); }
NTSTATUS ProposeCaptureFormat2(PPCPROPERTY_REQUEST request) { return ProposeFormat2(request, kWaveCaptureStreamPin); }

const PCPROPERTY_ITEM g_waveRenderProperties[] = {
    {&KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT, KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
     ProposeRenderFormat},
    {&KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT2, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
     ProposeRenderFormat2},
};
const PCPROPERTY_ITEM g_waveCaptureProperties[] = {
    {&KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT, KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
     ProposeCaptureFormat},
    {&KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT2, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
     ProposeCaptureFormat2},
};
DEFINE_PCAUTOMATION_TABLE_PROP(g_waveRenderAutomation, g_waveRenderProperties);
DEFINE_PCAUTOMATION_TABLE_PROP(g_waveCaptureAutomation, g_waveCaptureProperties);

// KSEVENT_PINCAPS_FORMATCHANGE on a streaming pin: Windows listens, and re-reads the pin's formats
// when the cable's format changes (NotifyCableFormatChange).
NTSTATUS FormatChangeEvent(PPCEVENT_REQUEST request);
const PCEVENT_ITEM g_streamPinEvents[] = {
    {&KSEVENTSETID_PinCapsChange, KSEVENT_PINCAPS_FORMATCHANGE, KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT,
     FormatChangeEvent},
};
DEFINE_PCAUTOMATION_TABLE_EVENT(g_streamPinAutomation, g_streamPinEvents);

#define WHA_FILTER(automation, pins, nodes, connections, categories)                                       \
  {0, automation, sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(pins), pins, sizeof(PCNODE_DESCRIPTOR), nodes, \
   SIZEOF_ARRAY(connections), connections, SIZEOF_ARRAY(categories), categories}
#define WHA_NODES(nodes) SIZEOF_ARRAY(nodes), nodes
#define WHA_NO_NODES 0, nullptr

PCFILTER_DESCRIPTOR g_waveRenderFilter =
    WHA_FILTER(&g_waveRenderAutomation, g_waveRenderPins, WHA_NO_NODES, g_passThrough, g_waveCategories);
PCFILTER_DESCRIPTOR g_waveCaptureFilter =
    WHA_FILTER(&g_waveCaptureAutomation, g_waveCapturePins, WHA_NO_NODES, g_passThrough, g_waveCategories);
PCFILTER_DESCRIPTOR g_topoRenderFilter =
    WHA_FILTER(nullptr, g_topoRenderPins, WHA_NODES(g_topoRenderNodes), g_topoConnections, g_topoCategories);
PCFILTER_DESCRIPTOR g_topoCaptureFilter =
    WHA_FILTER(nullptr, g_topoCapturePins, WHA_NODES(g_topoCaptureNodes), g_topoConnections, g_topoCategories);

// The format for a data range intersection on a streaming pin: the cable's one format as a
// WAVEFORMATEXTENSIBLE, if `clientRange` allows it. PortCls' default answer for a float range is a PCM
// tag with a float subformat, which no one can open. A client's bit range may name the container (32)
// or the valid bits (24) of 24-in-32.
NTSTATUS StreamIntersection(WHACable& cable, PKSDATARANGE clientRange, ULONG outputLength, PVOID result,
                            PULONG resultLength) {
  const WHACableFormat cf = CurrentFormat(cable);
  if (IsEqualGUIDAligned(clientRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
      clientRange->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
    const auto* client = reinterpret_cast<const KSDATARANGE_AUDIO*>(clientRange);
    const ULONG container = SampleBytes(cf.kind) * 8, valid = ValidBits(cf.kind);
    const auto inBits = [client](ULONG bits) {
      return client->MinimumBitsPerSample <= bits && client->MaximumBitsPerSample >= bits;
    };
    if (client->MaximumChannels < cf.channels || (!inBits(container) && !inBits(valid)) ||
        client->MinimumSampleFrequency > cf.rate || client->MaximumSampleFrequency < cf.rate)
      return STATUS_NO_MATCH;
  }
  const ULONG size = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
  *resultLength = size;
  if (!outputLength) return STATUS_BUFFER_OVERFLOW;  // size query
  if (outputLength < size) return STATUS_BUFFER_TOO_SMALL;
  FillFormat(static_cast<KSDATAFORMAT_WAVEFORMATEXTENSIBLE*>(result), cf);
  return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------
// Topology miniport: two bridge pins, the endpoint's name and type (speaker / microphone).

class WHATopologyMiniport : public IMiniportTopology, public CUnknown {
 public:
  DECLARE_STD_UNKNOWN();
  // The side's filter, with the endpoint pin named after the cable ("WinHookAudio Output/Input <n>").
  WHATopologyMiniport(PUNKNOWN outer, ULONG cable, bool capture) : CUnknown(outer), cable_(cable), capture_(capture) {
    const PCFILTER_DESCRIPTOR& side = capture ? g_topoCaptureFilter : g_topoRenderFilter;
    static_assert(SIZEOF_ARRAY(g_topoRenderPins) == 2 && SIZEOF_ARRAY(g_topoCapturePins) == 2, "pins_ size");
    RtlCopyMemory(pins_, side.Pins, sizeof(pins_));
    pins_[EndpointPin()].KsPinDescriptor.Name = capture ? &kInputNames[cable] : &kOutputNames[cable];
    filter_ = side;
    filter_.Pins = pins_;
  }
  IMP_IMiniportTopology;

  // The miniport a filter or node property request is for.
  static WHATopologyMiniport* Of(PPCPROPERTY_REQUEST request) {
    return static_cast<WHATopologyMiniport*>(static_cast<IMiniportTopology*>(request->MajorTarget));
  }
  WHACableLevel& Level() const { return capture_ ? g_cables[cable_].recordLevel : g_cables[cable_].playLevel; }
  KSPIN_LOCK& Lock() const { return g_cables[cable_].lock; }
  ULONG Channels() const { return CurrentFormat(g_cables[cable_]).channels; }  // the endpoint's, for its volume
  ULONG EndpointPin() const { return capture_ ? kTopoCaptureMicPin : kTopoRenderSpeakerPin; }

 private:
  ULONG cable_;
  bool capture_;
  PCPIN_DESCRIPTOR pins_[2];
  PCFILTER_DESCRIPTOR filter_;
};

STDMETHODIMP_(NTSTATUS) WHATopologyMiniport::NonDelegatingQueryInterface(REFIID iid, PVOID* object) {
  if (IsEqualGUIDAligned(iid, IID_IUnknown)) {
    *object = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
  } else if (IsEqualGUIDAligned(iid, IID_IMiniport)) {
    *object = PVOID(PMINIPORT(this));
  } else if (IsEqualGUIDAligned(iid, IID_IMiniportTopology)) {
    *object = PVOID(PMINIPORTTOPOLOGY(this));
  } else {
    *object = nullptr;
    return STATUS_INVALID_PARAMETER;
  }
  PUNKNOWN(*object)->AddRef();
  return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) WHATopologyMiniport::Init(PUNKNOWN, PRESOURCELIST, PPORTTOPOLOGY) { return STATUS_SUCCESS; }

STDMETHODIMP_(NTSTATUS) WHATopologyMiniport::GetDescription(PPCFILTER_DESCRIPTOR* description) {
  *description = &filter_;
  return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
WHATopologyMiniport::DataRangeIntersection(ULONG, PKSDATARANGE, PKSDATARANGE, ULONG, PVOID, PULONG) {
  return STATUS_NOT_IMPLEMENTED;  // PortCls' default handler
}

// A BASICSUPPORT reply for a per-channel stepped range (volume, mute) of `channels` channels: the
// description, and the ranges when they fit.
NTSTATUS SteppedRangeSupport(PPCPROPERTY_REQUEST request, ULONG channels, ULONG type, LONG minimum, LONG maximum,
                             LONG step) {
  const ULONG full =
      sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER) + channels * sizeof(KSPROPERTY_STEPPING_LONG);
  if (request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
    auto* d = static_cast<PKSPROPERTY_DESCRIPTION>(request->Value);
    d->AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;
    d->DescriptionSize = full;
    d->PropTypeSet.Set = KSPROPTYPESETID_General;
    d->PropTypeSet.Id = type;
    d->PropTypeSet.Flags = 0;
    d->MembersListCount = 1;
    d->Reserved = 0;
    if (request->ValueSize >= full) {
      auto* members = reinterpret_cast<PKSPROPERTY_MEMBERSHEADER>(d + 1);
      members->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
      members->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
      members->MembersCount = channels;
      members->Flags = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL;
      auto* ranges = reinterpret_cast<PKSPROPERTY_STEPPING_LONG>(members + 1);
      for (ULONG i = 0; i < channels; ++i) {
        ranges[i].SteppingDelta = ULONG(step);
        ranges[i].Reserved = 0;
        ranges[i].Bounds.SignedMinimum = minimum;
        ranges[i].Bounds.SignedMaximum = maximum;
      }
      request->ValueSize = full;
    } else {
      request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
    }
  } else if (request->ValueSize >= sizeof(ULONG)) {
    *static_cast<PULONG>(request->Value) = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;
    request->ValueSize = sizeof(ULONG);
  } else {
    request->ValueSize = 0;
    return STATUS_BUFFER_TOO_SMALL;
  }
  return STATUS_SUCCESS;
}

// The linear gain of `volume` (1/65536 dB, a whole step in kVolumeMin..0): 0.5 dB per step down.
float VolumeGain(LONG volume) {
  float gain = 1.0f;
  for (LONG v = volume; v < 0; v += kVolumeStep) gain *= 0.94406087f;  // 10^(-0.5/20)
  return gain;
}

// KSPROPERTY_AUDIO_VOLUMELEVEL / MUTE on the volume and mute nodes. The instance is the channel (or
// all channels, ~0 on a SET).
NTSTATUS TopologyNodeProperty(PPCPROPERTY_REQUEST request) {
  WHATopologyMiniport* miniport = WHATopologyMiniport::Of(request);
  const bool isVolume = request->PropertyItem->Id == KSPROPERTY_AUDIO_VOLUMELEVEL;
  if (request->Node != (isVolume ? kVolumeNode : kMuteNode)) return STATUS_INVALID_DEVICE_REQUEST;
  if (request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
    const ULONG channels = miniport->Channels();
    return isVolume ? SteppedRangeSupport(request, channels, VT_I4, kVolumeMin, kVolumeMax, kVolumeStep)
                    : SteppedRangeSupport(request, channels, VT_BOOL, 0, 1, 1);
  }
  if (request->InstanceSize < sizeof(ULONG)) return STATUS_INVALID_PARAMETER;
  const ULONG channel = *static_cast<const ULONG*>(request->Instance);
  constexpr ULONG kAllChannels = ~0UL;
  if (channel >= WHACableLevel::kChannels && channel != kAllChannels) return STATUS_INVALID_PARAMETER;
  const ULONG valueSize = isVolume ? sizeof(LONG) : sizeof(BOOL);
  if (!request->ValueSize) {
    request->ValueSize = valueSize;
    return STATUS_BUFFER_OVERFLOW;
  }
  if (request->ValueSize < valueSize) return STATUS_BUFFER_TOO_SMALL;
  const ULONG first = channel == kAllChannels ? 0 : channel;
  const ULONG end = channel == kAllChannels ? WHACableLevel::kChannels : channel + 1;
  WHACableLevel& level = miniport->Level();
  KIRQL irql;
  if (request->Verb & KSPROPERTY_TYPE_GET) {
    KeAcquireSpinLock(&miniport->Lock(), &irql);
    if (isVolume)
      *static_cast<PLONG>(request->Value) = level.volume[first];
    else
      *static_cast<PBOOL>(request->Value) = level.mute[first];
    KeReleaseSpinLock(&miniport->Lock(), irql);
    request->ValueSize = valueSize;
    return STATUS_SUCCESS;
  }
  if (!(request->Verb & KSPROPERTY_TYPE_SET)) return STATUS_INVALID_DEVICE_REQUEST;
  LONG volume = *static_cast<const LONG*>(request->Value);  // or the mute BOOL (same size)
  if (isVolume) {
    volume = volume < kVolumeMin ? kVolumeMin : volume > kVolumeMax ? kVolumeMax : volume;
    volume = kVolumeMin + (volume - kVolumeMin + kVolumeStep / 2) / kVolumeStep * kVolumeStep;
  }
  KeAcquireSpinLock(&miniport->Lock(), &irql);
  for (ULONG c = first; c < end; ++c) {
    if (isVolume) level.volume[c] = volume;
    else level.mute[c] = volume ? TRUE : FALSE;
    level.gain[c] = level.mute[c] ? 0.0f : VolumeGain(level.volume[c]);  // at most 192 multiplies each
  }
  KeReleaseSpinLock(&miniport->Lock(), irql);
  return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------
// WaveRT stream: the audio engine's cyclic buffer advances at the nominal rate by the performance
// counter. Every millisecond (and whenever the engine asks the position) the frames passed since the
// last copy move between the buffer and the cable's ring: playback buffer -> play ring; record ring ->
// recording buffer. Integer formats convert through a small float scratch.

class WHAWaveStream : public IMiniportWaveRTStream, public CUnknown {
 public:
  DECLARE_STD_UNKNOWN();
  WHAWaveStream(PUNKNOWN outer) : CUnknown(outer) {}
  ~WHAWaveStream();
  IMP_IMiniportWaveRTStream;

  NTSTATUS Init(WHACable* cable, bool capture, PPORTWAVERTSTREAM portStream, const StreamFormat& format);

 private:
  static EXT_CALLBACK TimerCallback;
  void UpdateLocked();  // cable_->lock held
  ULONGLONG ElapsedFramesLocked() const;

  WHACable* cable_ = nullptr;
  bool capture_ = false;
  PPORTWAVERTSTREAM portStream_ = nullptr;
  ULONG rate_ = 0;
  ULONG channels_ = 0;
  WHASampleKind kind_ = WHASampleKind::Float32;
  ULONG frameBytes_ = 0;
  PEX_TIMER timer_ = nullptr;
  static constexpr ULONG kScratchFrames = 256;
  float scratch_[kCableChannels * kScratchFrames] = {};  // integer <-> float conversion (under cable_->lock)
  // Guarded by cable_->lock:
  PMDL mdl_ = nullptr;
  BYTE* buffer_ = nullptr;
  ULONG bufferFrames_ = 0;
  KSSTATE state_ = KSSTATE_STOP;
  bool running_ = false;
  LONGLONG qpcFrequency_ = 0;
  LONGLONG runStartQpc_ = 0;      // performance counter when RUN began
  ULONGLONG framesAtRun_ = 0;     // stream position (frames) when RUN began
  ULONGLONG framesDone_ = 0;      // stream position (frames) copied up to
};

NTSTATUS WHAWaveStream::Init(WHACable* cable, bool capture, PPORTWAVERTSTREAM portStream,
                             const StreamFormat& format) {
  cable_ = cable;
  capture_ = capture;
  portStream_ = portStream;
  portStream_->AddRef();
  rate_ = format.rate;
  channels_ = format.channels;
  kind_ = format.kind;
  frameBytes_ = FrameBytes(format.kind, format.channels);
  LARGE_INTEGER frequency;
  KeQueryPerformanceCounter(&frequency);
  qpcFrequency_ = frequency.QuadPart;
  timer_ = ExAllocateTimer(TimerCallback, this, EX_TIMER_HIGH_RESOLUTION);
  return timer_ ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

WHAWaveStream::~WHAWaveStream() {
  if (timer_) ExDeleteTimer(timer_, TRUE, TRUE, nullptr);  // cancels and waits for a running callback
  if (cable_) {
    KIRQL irql;
    KeAcquireSpinLock(&cable_->lock, &irql);
    running_ = false;
    (capture_ ? cable_->recordRate : cable_->playRate) = 0;
    KeReleaseSpinLock(&cable_->lock, irql);
  }
  if (mdl_) FreeAudioBuffer(mdl_, bufferFrames_ * frameBytes_);
  if (portStream_) portStream_->Release();
}

STDMETHODIMP_(NTSTATUS) WHAWaveStream::NonDelegatingQueryInterface(REFIID iid, PVOID* object) {
  if (IsEqualGUIDAligned(iid, IID_IUnknown)) {
    *object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
  } else if (IsEqualGUIDAligned(iid, IID_IMiniportWaveRTStream)) {
    *object = PVOID(PMINIPORTWAVERTSTREAM(this));
  } else {
    *object = nullptr;
    return STATUS_INVALID_PARAMETER;
  }
  PUNKNOWN(*object)->AddRef();
  return STATUS_SUCCESS;
}

ULONGLONG WHAWaveStream::ElapsedFramesLocked() const {
  LARGE_INTEGER now = KeQueryPerformanceCounter(nullptr);
  const ULONGLONG ticks = now.QuadPart > runStartQpc_ ? ULONGLONG(now.QuadPart - runStartQpc_) : 0;
  const ULONGLONG frequency = ULONGLONG(qpcFrequency_);
  return ticks / frequency * rate_ + ticks % frequency * rate_ / frequency;
}

void WHAWaveStream::UpdateLocked() {
  if (!running_ || !buffer_ || !bufferFrames_) return;
  const ULONGLONG target = framesAtRun_ + ElapsedFramesLocked();
  if (target <= framesDone_) return;
  ULONGLONG pending = target - framesDone_;
  if (pending > bufferFrames_) {  // fell behind a whole buffer (should not happen): skip the lost part
    framesDone_ = target - bufferFrames_;
    pending = bufferFrames_;
  }
  const unsigned prime = CablePrime(*cable_), slack = CableSlack(*cable_), grow = CableGrow(*cable_),
                 maxPrime = CableMaxPrime(*cable_);
  const float* gain = (capture_ ? cable_->recordLevel : cable_->playLevel).gain;
  const ULONG ch = channels_;
  bool unity = true;
  for (ULONG c = 0; c < ch; ++c) unity = unity && gain[c] == 1.0f;
  while (pending) {
    const ULONG offset = ULONG(framesDone_ % bufferFrames_);
    ULONG chunk = bufferFrames_ - offset;
    if (chunk > pending) chunk = ULONG(pending);
    BYTE* frames = buffer_ + ULONGLONG(offset) * frameBytes_;
    if (kind_ == WHASampleKind::Float32 && unity) {  // bit-exact, straight between buffer and ring
      if (capture_)
        cable_->record.read(reinterpret_cast<float*>(frames), chunk, ch, prime, slack, grow, maxPrime);
      else
        cable_->play.write(reinterpret_cast<const float*>(frames), chunk, ch);
    } else {
      for (ULONG done = 0; done < chunk;) {
        const ULONG n = chunk - done < kScratchFrames ? chunk - done : kScratchFrames;
        BYTE* at = frames + ULONGLONG(done) * frameBytes_;
        if (capture_) cable_->record.read(scratch_, n, ch, prime, slack, grow, maxPrime);
        else SamplesToFloat(at, scratch_, n * ch, kind_);
        if (!unity)
          for (ULONG i = 0; i < n; ++i)
            for (ULONG c = 0; c < ch; ++c) scratch_[ch * i + c] *= gain[c];
        if (capture_) FloatToSamples(scratch_, at, n * ch, kind_);
        else cable_->play.write(scratch_, n, ch);
        done += n;
      }
    }
    framesDone_ += chunk;
    pending -= chunk;
  }
}

void WHAWaveStream::TimerCallback(PEX_TIMER, PVOID context) {
  auto* self = static_cast<WHAWaveStream*>(context);
  KIRQL irql;
  KeAcquireSpinLock(&self->cable_->lock, &irql);
  self->UpdateLocked();
  KeReleaseSpinLock(&self->cable_->lock, irql);
}

STDMETHODIMP_(NTSTATUS) WHAWaveStream::SetFormat(PKSDATAFORMAT) {
  return STATUS_NOT_SUPPORTED;  // one format per stream; the engine reopens to change it
}

STDMETHODIMP_(NTSTATUS) WHAWaveStream::SetState(KSSTATE state) {
  bool startTimer = false, stopTimer = false;
  KIRQL irql;
  KeAcquireSpinLock(&cable_->lock, &irql);
  if (state == KSSTATE_RUN && !running_) {
    runStartQpc_ = KeQueryPerformanceCounter(nullptr).QuadPart;
    framesAtRun_ = framesDone_;
    running_ = true;
    (capture_ ? cable_->recordRate : cable_->playRate) = rate_;
    startTimer = true;
  } else if (state != KSSTATE_RUN && running_) {
    UpdateLocked();  // up to the moment it stops
    running_ = false;
    stopTimer = true;
  }
  if (state == KSSTATE_STOP) {
    framesDone_ = framesAtRun_ = 0;
    (capture_ ? cable_->recordRate : cable_->playRate) = 0;
  }
  state_ = state;
  KeReleaseSpinLock(&cable_->lock, irql);
  if (startTimer) ExSetTimer(timer_, -10000LL, 10000LL, nullptr);  // 1 ms, periodic (100 ns units)
  if (stopTimer) ExCancelTimer(timer_, nullptr);  // a callback in flight finds running_ false
  return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) WHAWaveStream::GetPosition(PKSAUDIO_POSITION position) {
  KIRQL irql;
  KeAcquireSpinLock(&cable_->lock, &irql);
  UpdateLocked();
  const ULONGLONG bytes = bufferFrames_ ? framesDone_ % bufferFrames_ * frameBytes_ : 0;
  KeReleaseSpinLock(&cable_->lock, irql);
  position->PlayOffset = bytes;
  position->WriteOffset = bytes;
  return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
WHAWaveStream::AllocateAudioBuffer(ULONG requestedSize, PMDL* audioBufferMdl, ULONG* actualSize,
                                   ULONG* offsetFromFirstPage, MEMORY_CACHING_TYPE* cacheType) {
  const ULONG frames = requestedSize / frameBytes_;
  if (!frames) return STATUS_INVALID_PARAMETER;
  if (mdl_) return STATUS_DEVICE_BUSY;
  const ULONG bytes = frames * frameBytes_;
  PHYSICAL_ADDRESS highest;
  highest.QuadPart = MAXLONGLONG;
  PMDL mdl = portStream_->AllocatePagesForMdl(highest, bytes);
  if (!mdl) return STATUS_INSUFFICIENT_RESOURCES;
  if (MmGetMdlByteCount(mdl) < bytes) {
    portStream_->FreePagesFromMdl(mdl);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  auto* buffer = static_cast<BYTE*>(portStream_->MapAllocatedPages(mdl, MmCached));
  if (!buffer) {
    portStream_->FreePagesFromMdl(mdl);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  RtlZeroMemory(buffer, bytes);
  KIRQL irql;
  KeAcquireSpinLock(&cable_->lock, &irql);
  mdl_ = mdl;
  buffer_ = buffer;
  bufferFrames_ = frames;
  KeReleaseSpinLock(&cable_->lock, irql);
  *audioBufferMdl = mdl;
  *actualSize = bytes;
  *offsetFromFirstPage = 0;
  *cacheType = MmCached;
  return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID) WHAWaveStream::FreeAudioBuffer(PMDL audioBufferMdl, ULONG) {
  KIRQL irql;
  KeAcquireSpinLock(&cable_->lock, &irql);
  BYTE* buffer = audioBufferMdl == mdl_ ? buffer_ : nullptr;
  if (buffer) {
    mdl_ = nullptr;
    buffer_ = nullptr;
    bufferFrames_ = 0;
  }
  KeReleaseSpinLock(&cable_->lock, irql);
  if (!buffer) return;
  portStream_->UnmapAllocatedPages(buffer, audioBufferMdl);
  portStream_->FreePagesFromMdl(audioBufferMdl);
}

STDMETHODIMP_(VOID) WHAWaveStream::GetHWLatency(KSRTAUDIO_HWLATENCY* hwLatency) {
  hwLatency->FifoSize = 0;
  hwLatency->ChipsetDelay = 0;
  hwLatency->CodecDelay = 0;
}

STDMETHODIMP_(NTSTATUS) WHAWaveStream::GetPositionRegister(KSRTAUDIO_HWREGISTER*) { return STATUS_NOT_SUPPORTED; }
STDMETHODIMP_(NTSTATUS) WHAWaveStream::GetClockRegister(KSRTAUDIO_HWREGISTER*) { return STATUS_NOT_SUPPORTED; }

// ---------------------------------------------------------------------------------------------------
// Wave miniport: one streaming pin and one bridge pin.

class WHAWaveMiniport : public IMiniportWaveRT, public CUnknown {
 public:
  DECLARE_STD_UNKNOWN();
  // The side's filter, with the streaming pin listing the cable's data range and format-change event.
  WHAWaveMiniport(PUNKNOWN outer, ULONG cable, bool capture) : CUnknown(outer), cable_(cable), capture_(capture) {
    const PCFILTER_DESCRIPTOR& side = capture ? g_waveCaptureFilter : g_waveRenderFilter;
    static_assert(SIZEOF_ARRAY(g_waveRenderPins) == 2 && SIZEOF_ARRAY(g_waveCapturePins) == 2, "pins_ size");
    RtlCopyMemory(pins_, side.Pins, sizeof(pins_));
    PCPIN_DESCRIPTOR& stream = pins_[StreamPin()];
    stream.AutomationTable = &g_streamPinAutomation;
    stream.KsPinDescriptor.DataRangesCount = SIZEOF_ARRAY(g_cables[cable].ranges);
    stream.KsPinDescriptor.DataRanges = g_cables[cable].ranges;
    filter_ = side;
    filter_.Pins = pins_;
  }
  ~WHAWaveMiniport();
  IMP_IMiniportWaveRT;

  static WHAWaveMiniport* Of(PUNKNOWN majorTarget) {
    return static_cast<WHAWaveMiniport*>(static_cast<IMiniportWaveRT*>(majorTarget));
  }
  WHACable& Cable() const { return g_cables[cable_]; }
  ULONG StreamPin() const { return capture_ ? kWaveCaptureStreamPin : kWaveRenderStreamPin; }
  PPORTEVENTS Events() const { return events_; }

 private:
  ULONG cable_;
  bool capture_;
  PPORTEVENTS events_ = nullptr;  // our port's, from Init; the cable borrows it (WHACable::events)
  PCPIN_DESCRIPTOR pins_[2];
  PCFILTER_DESCRIPTOR filter_;
};

WHACable& CableOf(PUNKNOWN majorTarget) { return WHAWaveMiniport::Of(majorTarget)->Cable(); }

// The streaming pin's KSEVENT_PINCAPS_FORMATCHANGE: Windows enables it on the filter; PortCls keeps
// the entry on our port's list, which NotifyCableFormatChange signals.
NTSTATUS FormatChangeEvent(PPCEVENT_REQUEST request) {
  WHAWaveMiniport* miniport = WHAWaveMiniport::Of(request->MajorTarget);
  switch (request->Verb) {
    case PCEVENT_VERB_SUPPORT:
    case PCEVENT_VERB_REMOVE:
      return STATUS_SUCCESS;
    case PCEVENT_VERB_ADD:
      if (!request->EventEntry || !miniport->Events()) return STATUS_INVALID_PARAMETER;
      miniport->Events()->AddEventToEventList(request->EventEntry);
      return STATUS_SUCCESS;
    default:
      return STATUS_INVALID_PARAMETER;
  }
}

WHAWaveMiniport::~WHAWaveMiniport() {
  if (!events_) return;
  WHACable& c = Cable();
  KIRQL irql;
  KeAcquireSpinLock(&c.lock, &irql);
  c.events[capture_ ? 1 : 0] = nullptr;
  KeReleaseSpinLock(&c.lock, irql);
  events_->Release();
}

STDMETHODIMP_(NTSTATUS) WHAWaveMiniport::NonDelegatingQueryInterface(REFIID iid, PVOID* object) {
  if (IsEqualGUIDAligned(iid, IID_IUnknown)) {
    *object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
  } else if (IsEqualGUIDAligned(iid, IID_IMiniport)) {
    *object = PVOID(PMINIPORT(this));
  } else if (IsEqualGUIDAligned(iid, IID_IMiniportWaveRT)) {
    *object = PVOID(PMINIPORTWAVERT(this));
  } else {
    *object = nullptr;
    return STATUS_INVALID_PARAMETER;
  }
  PUNKNOWN(*object)->AddRef();
  return STATUS_SUCCESS;
}

// The port's events interface, to tell Windows when the cable's format changes. Without it the cable
// still streams; a format change then shows up only when Windows next rebuilds the endpoint.
STDMETHODIMP_(NTSTATUS) WHAWaveMiniport::Init(PUNKNOWN, PRESOURCELIST, PPORTWAVERT port) {
  PPORTEVENTS events = nullptr;
  if (!port || !NT_SUCCESS(port->QueryInterface(IID_IPortEvents, reinterpret_cast<PVOID*>(&events))) || !events)
    return STATUS_SUCCESS;
  events_ = events;
  WHACable& c = Cable();
  KIRQL irql;
  KeAcquireSpinLock(&c.lock, &irql);
  c.events[capture_ ? 1 : 0] = events;
  KeReleaseSpinLock(&c.lock, irql);
  return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) WHAWaveMiniport::GetDescription(PPCFILTER_DESCRIPTOR* description) {
  *description = &filter_;
  return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
WHAWaveMiniport::DataRangeIntersection(ULONG pin, PKSDATARANGE clientRange, PKSDATARANGE, ULONG outputLength,
                                       PVOID result, PULONG resultLength) {
  if (pin != StreamPin()) return STATUS_NOT_IMPLEMENTED;  // bridge pin: PortCls' default handler
  return StreamIntersection(Cable(), clientRange, outputLength, result, resultLength);
}

STDMETHODIMP_(NTSTATUS) WHAWaveMiniport::GetDeviceDescription(PDEVICE_DESCRIPTION description) {
  RtlZeroMemory(description, sizeof(DEVICE_DESCRIPTION));
  description->Version = DEVICE_DESCRIPTION_VERSION;
  description->Master = TRUE;
  description->ScatterGather = TRUE;
  description->Dma32BitAddresses = TRUE;
  description->InterfaceType = PCIBus;
  description->MaximumLength = 0xFFFFFFFE;
  return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
WHAWaveMiniport::NewStream(PMINIPORTWAVERTSTREAM* stream, PPORTWAVERTSTREAM portStream, ULONG pin,
                           BOOLEAN capture, PKSDATAFORMAT dataFormat) {
  *stream = nullptr;
  if (pin != StreamPin() || bool(capture) != capture_) return STATUS_INVALID_PARAMETER;
  // Only the cable's format: one opened in an older format (just before a change) is refused, and
  // Windows reopens in the new one after the format-change event.
  StreamFormat format;
  if (!ParseFormat(dataFormat, &format) || !Offers(CurrentFormat(Cable()), format)) return STATUS_NO_MATCH;
  auto* s = new (POOL_FLAG_NON_PAGED, kPoolTag) WHAWaveStream(nullptr);
  if (!s) return STATUS_INSUFFICIENT_RESOURCES;
  s->AddRef();
  const NTSTATUS status = s->Init(&Cable(), capture_, portStream, format);
  if (!NT_SUCCESS(status)) {
    s->Release();
    return status;
  }
  *stream = PMINIPORTWAVERTSTREAM(s);  // our reference goes to the caller
  return STATUS_SUCCESS;
}

}  // namespace

bool SetCableFormatLocked(WHACable& c, const WHACableFormat& format) {
  const bool changed = format.rate != c.format.rate || format.channels != c.format.channels || format.kind != c.format.kind;
  c.format = format;
  KSDATARANGE_AUDIO& r = c.range;
  r.DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
  r.DataRange.Flags = 0;
  r.DataRange.SampleSize = 0;
  r.DataRange.Reserved = 0;
  r.DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
  r.DataRange.SubFormat = format.kind == WHASampleKind::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
  r.DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
  r.MaximumChannels = format.channels;
  r.MinimumBitsPerSample = r.MaximumBitsPerSample = SampleBytes(format.kind) * 8;
  r.MinimumSampleFrequency = r.MaximumSampleFrequency = format.rate;
  c.ranges[0] = &r.DataRange;
  return changed;
}

void NotifyCableFormatChange(WHACable& c) {
  PPORTEVENTS events[2] = {};
  KIRQL irql;
  KeAcquireSpinLock(&c.lock, &irql);
  for (int side = 0; side < 2; ++side)
    if ((events[side] = c.events[side]) != nullptr) events[side]->AddRef();  // the miniport holds one: alive
  KeReleaseSpinLock(&c.lock, irql);
  GUID set = KSEVENTSETID_PinCapsChange;
  for (int side = 0; side < 2; ++side) {
    if (!events[side]) continue;
    events[side]->GenerateEventList(&set, KSEVENT_PINCAPS_FORMATCHANGE, TRUE,
                                    side ? kWaveCaptureStreamPin : kWaveRenderStreamPin, FALSE, ULONG(-1));
    events[side]->Release();
  }
}

NTSTATUS NewWaveMiniport(PUNKNOWN* out, ULONG cable, bool capture) {
  auto* m = new (POOL_FLAG_NON_PAGED, kPoolTag) WHAWaveMiniport(nullptr, cable, capture);
  if (!m) return STATUS_INSUFFICIENT_RESOURCES;
  *out = PUNKNOWN(PMINIPORTWAVERT(m));
  (*out)->AddRef();
  return STATUS_SUCCESS;
}

NTSTATUS NewTopologyMiniport(PUNKNOWN* out, ULONG cable, bool capture) {
  auto* m = new (POOL_FLAG_NON_PAGED, kPoolTag) WHATopologyMiniport(nullptr, cable, capture);
  if (!m) return STATUS_INSUFFICIENT_RESOURCES;
  *out = PUNKNOWN(PMINIPORTTOPOLOGY(m));
  (*out)->AddRef();
  return STATUS_SUCCESS;
}

}  // namespace wha
