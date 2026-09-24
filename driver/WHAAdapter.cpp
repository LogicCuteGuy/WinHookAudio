// WHAAdapter - WinHookAudio.sys entry: PortCls adapter (registers each cable's wave and topology
// filters) and the control device the Worker exchanges cable audio through.
// Vocabulary: Virtual Cable, Worker.

#include <initguid.h>  // defines the PortCls interface IDs in this translation unit

#include "WHADriver.h"

#include <wdmsec.h>

#include "../common/virtual/WHACableProtocol.h"

extern "C" NTKERNELAPI HANDLE PsGetCurrentProcessId();  // ntddk.h, which PortCls' wdm.h does not include

// Kernel C++ allocation: nonpaged, zeroed (ExAllocatePool2).
void* __cdecl operator new(size_t size, POOL_FLAGS flags, ULONG tag) {
  return size ? ExAllocatePool2(flags, size, tag) : nullptr;
}
void* __cdecl operator new[](size_t size, POOL_FLAGS flags, ULONG tag) {
  return size ? ExAllocatePool2(flags, size, tag) : nullptr;
}
// operator delete(void*) comes from stdunk.lib (ExFreePool).
void __cdecl operator delete[](void* p) {
  if (p) ExFreePool(p);
}
void __cdecl operator delete(void* p, size_t) {
  if (p) ExFreePool(p);
}
void __cdecl operator delete(void* p, POOL_FLAGS, ULONG) {  // matches the placement new on a throwing constructor
  if (p) ExFreePool(p);
}

namespace wha {

WHACable* g_cables = nullptr;

namespace {

// {C598DC03-4CEF-4072-8DCF-EAD9536465CF}: the control device's class (IoCreateDeviceSecure).
const GUID kControlClass = {0xc598dc03, 0x4cef, 0x4072, {0x8d, 0xcf, 0xea, 0xd9, 0x53, 0x64, 0x65, 0xcf}};

PDEVICE_OBJECT g_control = nullptr;  // created with the first adapter device, deleted with it
PDRIVER_UNLOAD g_portClsUnload = nullptr;

// Subdevice names = the reference strings in WinHookAudio.inf's AddInterface lines. Windows keys each
// endpoint (and the name it gave it) by these: renaming them makes Windows create the endpoints anew.
#define WHA_SUBDEVICES(n)                                                                                  \
  {const_cast<PWSTR>(L"OutputWave" #n), const_cast<PWSTR>(L"OutputTopology" #n), const_cast<PWSTR>(L"InputWave" #n), \
   const_cast<PWSTR>(L"InputTopology" #n)}
const PWSTR kSubdeviceNames[kCables][kSubdevicesPerCable] = {
    WHA_SUBDEVICES(1), WHA_SUBDEVICES(2), WHA_SUBDEVICES(3), WHA_SUBDEVICES(4),
    WHA_SUBDEVICES(5), WHA_SUBDEVICES(6), WHA_SUBDEVICES(7), WHA_SUBDEVICES(8),
};
#undef WHA_SUBDEVICES

// Creates a port + miniport and registers them as subdevice `name`. Returns the port (a reference the
// caller releases) for physical connections.
NTSTATUS InstallSubdevice(PDEVICE_OBJECT device, PIRP irp, PWSTR name, REFGUID portClass, PUNKNOWN miniport,
                          PRESOURCELIST resources, PUNKNOWN* portOut) {
  PPORT port = nullptr;
  NTSTATUS status = PcNewPort(&port, portClass);
  if (!NT_SUCCESS(status)) return status;
  status = port->Init(device, irp, miniport, nullptr, resources);
  if (NT_SUCCESS(status)) status = PcRegisterSubdevice(device, name, port);
  if (!NT_SUCCESS(status)) {
    port->Release();
    return status;
  }
  *portOut = port;
  return STATUS_SUCCESS;
}

// One side of one cable: wave filter + topology filter, connected by their bridge pins.
NTSTATUS InstallCableSide(PDEVICE_OBJECT device, PIRP irp, PRESOURCELIST resources, ULONG cable, bool capture) {
  PUNKNOWN waveMiniport = nullptr, topoMiniport = nullptr, wavePort = nullptr, topoPort = nullptr;
  NTSTATUS status = NewWaveMiniport(&waveMiniport, cable, capture);
  if (NT_SUCCESS(status)) status = NewTopologyMiniport(&topoMiniport, cable, capture);
  const ULONG base = capture ? 2 : 0;
  if (NT_SUCCESS(status))
    status = InstallSubdevice(device, irp, kSubdeviceNames[cable][base], CLSID_PortWaveRT, waveMiniport, resources,
                              &wavePort);
  if (NT_SUCCESS(status))
    status = InstallSubdevice(device, irp, kSubdeviceNames[cable][base + 1], CLSID_PortTopology, topoMiniport,
                              resources, &topoPort);
  if (NT_SUCCESS(status)) {
    status = capture ? PcRegisterPhysicalConnection(device, topoPort, kTopoCaptureWavePin, wavePort,
                                                    kWaveCaptureBridgePin)
                     : PcRegisterPhysicalConnection(device, wavePort, kWaveRenderBridgePin, topoPort,
                                                    kTopoRenderWavePin);
  }
  if (topoPort) topoPort->Release();
  if (wavePort) wavePort->Release();
  if (topoMiniport) topoMiniport->Release();
  if (waveMiniport) waveMiniport->Release();
  return status;
}

NTSTATUS StartDevice(PDEVICE_OBJECT device, PIRP irp, PRESOURCELIST resources) {
  for (ULONG cable = 0; cable < kCables; ++cable) {
    NTSTATUS status = InstallCableSide(device, irp, resources, cable, false);
    if (NT_SUCCESS(status)) status = InstallCableSide(device, irp, resources, cable, true);
    if (!NT_SUCCESS(status)) return status;
  }
  return STATUS_SUCCESS;
}

NTSTATUS CreateControlDevice(PDRIVER_OBJECT driver) {
  if (g_control) return STATUS_SUCCESS;
  UNICODE_STRING name, link;
  RtlInitUnicodeString(&name, WHA_CABLE_DEVICE_NAME);
  RtlInitUnicodeString(&link, WHA_CABLE_LINK_NAME);
  // The DAW runs as a normal user: everyone may read and write (exchange audio), nothing more.
  // Exclusive: one Worker at a time (a second one would take half of each cable's frames).
  PDEVICE_OBJECT control = nullptr;
  NTSTATUS status = IoCreateDeviceSecure(driver, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, TRUE,
                                         &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R, &kControlClass, &control);
  if (!NT_SUCCESS(status)) return status;
  status = IoCreateSymbolicLink(&link, &name);
  if (!NT_SUCCESS(status)) {
    IoDeleteDevice(control);
    return status;
  }
  control->Flags |= DO_BUFFERED_IO;
  control->Flags &= ~DO_DEVICE_INITIALIZING;
  g_control = control;
  return STATUS_SUCCESS;
}

void DeleteControlDevice() {
  if (!g_control) return;
  UNICODE_STRING link;
  RtlInitUnicodeString(&link, WHA_CABLE_LINK_NAME);
  IoDeleteSymbolicLink(&link);
  IoDeleteDevice(g_control);
  g_control = nullptr;
}

NTSTATUS Exchange(PIRP irp, PIO_STACK_LOCATION stack) {
  const ULONG inLength = stack->Parameters.DeviceIoControl.InputBufferLength;
  const ULONG outLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
  auto* io = static_cast<BYTE*>(irp->AssociatedIrp.SystemBuffer);
  if (!io || inLength < sizeof(WHACableExchange) || outLength < sizeof(WHACableExchange))
    return STATUS_BUFFER_TOO_SMALL;
  WHACableExchange request;
  RtlCopyMemory(&request, io, sizeof(request));
  if (request.protocol != WHA_CABLE_PROTOCOL) return STATUS_REVISION_MISMATCH;
  if (request.cable >= kCables || request.frames > WHA_CABLE_MAX_FRAMES) return STATUS_INVALID_PARAMETER;
  // A probe (rate 0) keeps the format and moves no audio; otherwise the format must be one we stream.
  const bool probe = request.rate == 0;
  if (probe ? request.frames || request.channels
            : request.rate < 8000 || request.rate > 384000 || request.channels < 1 ||
                  request.channels > WHA_CABLE_MAX_CHANNELS || !IsValidCableFormat(request.format))
    return STATUS_INVALID_PARAMETER;
  const ULONG audioBytes = request.frames * request.channels * sizeof(float);
  if ((request.hasRecord && inLength < sizeof(WHACableExchange) + audioBytes) ||
      outLength < sizeof(WHACableExchange) + audioBytes)
    return STATUS_BUFFER_TOO_SMALL;

  // In and out share the system buffer: take the recording frames before the playback frames land.
  auto* audio = reinterpret_cast<float*>(io + sizeof(WHACableExchange));
  WHACable& c = g_cables[request.cable];
  WHACableExchange reply = {};
  bool formatChanged = false;
  KIRQL irql;
  KeAcquireSpinLock(&c.lock, &irql);
  if (!probe) {
    WHACableFormat format;
    format.rate = request.rate;
    format.channels = request.channels;
    format.kind = static_cast<WHASampleKind>(request.format);
    formatChanged = SetCableFormatLocked(c, format);
  }
  if (request.frames) {
    c.workerFrames = request.frames;
    c.record.write(request.hasRecord ? audio : nullptr, request.frames, request.channels);
    c.play.read(audio, request.frames, request.channels, CablePrime(c), CableSlack(c));
  }
  reply.rate = c.format.rate;
  reply.channels = c.format.channels;
  reply.format = static_cast<unsigned>(c.format.kind);
  reply.playRate = c.playRate;
  reply.recordRate = c.recordRate;
  reply.playFill = c.play.fill();
  reply.recordFill = c.record.fill();
  reply.playUnderruns = c.play.underruns();
  reply.playDrops = c.play.drops();
  reply.recordUnderruns = c.record.underruns();
  reply.recordDrops = c.record.drops();
  KeReleaseSpinLock(&c.lock, irql);
  if (formatChanged) NotifyCableFormatChange(c);  // Windows reopens its streams in the new format
  reply.protocol = WHA_CABLE_PROTOCOL;
  reply.cable = request.cable;
  reply.frames = request.frames;
  reply.hasRecord = request.hasRecord;
  reply.cables = kCables;
  RtlCopyMemory(io, &reply, sizeof(reply));
  irp->IoStatus.Information = sizeof(WHACableExchange) + audioBytes;
  return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------
// KS request log (diagnostic, IOCTL_WHA_KS_LOG): every open and device control on the audio filters.

WHAKsLogEntry* g_log = nullptr;  // WHA_KS_LOG_ENTRIES, nonpaged
ULONG g_logTotal = 0;
KSPIN_LOCK g_logLock;

void CopyName(unsigned short* out, const UNICODE_STRING& name) {
  const ULONG n = min(ULONG(name.Length / sizeof(WCHAR)), ULONG(RTL_NUMBER_OF(WHAKsLogEntry{}.name) - 1));
  for (ULONG i = 0; i < n; ++i) out[i] = name.Buffer[i];
}

// Fills `e` from an IRP before PortCls handles it (the IRP may be gone after). False: not logged.
bool DescribeRequest(PIRP irp, WHAKsLogEntry* e) {
  PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
  if (stack->MajorFunction != IRP_MJ_CREATE && stack->MajorFunction != IRP_MJ_DEVICE_CONTROL) return false;
  RtlZeroMemory(e, sizeof(*e));
  e->major = stack->MajorFunction;
  e->process = HandleToULong(PsGetCurrentProcessId());  // top of the stack: the caller's context
  PFILE_OBJECT file = stack->FileObject;
  if (file) {
    PFILE_OBJECT filter = file->RelatedFileObject ? file->RelatedFileObject : file;
    e->onPin = file->RelatedFileObject ? 1 : 0;
    CopyName(e->name, filter->FileName);
  }
  if (e->major == IRP_MJ_CREATE) return true;
  const auto& io = stack->Parameters.DeviceIoControl;
  e->ioctl = io.IoControlCode;
  e->inLength = io.InputBufferLength;
  e->outLength = io.OutputBufferLength;
  if ((e->ioctl == IOCTL_KS_PROPERTY || e->ioctl == IOCTL_KS_METHOD || e->ioctl == IOCTL_KS_ENABLE_EVENT) &&
      e->inLength >= sizeof(KSIDENTIFIER) && io.Type3InputBuffer) {
    BYTE in[sizeof(KSIDENTIFIER) + 8] = {};
    const ULONG n = min(e->inLength, ULONG(sizeof(in)));
    __try {
      if (irp->RequestorMode == UserMode) ProbeForRead(io.Type3InputBuffer, n, 1);
      RtlCopyMemory(in, io.Type3InputBuffer, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return true;
    }
    const auto* id = reinterpret_cast<const KSIDENTIFIER*>(in);
    RtlCopyMemory(e->propertySet, &id->Set, sizeof(GUID));
    e->propertyId = id->Id;
    e->propertyFlags = id->Flags;
    RtlCopyMemory(e->instance, in + sizeof(KSIDENTIFIER), n - sizeof(KSIDENTIFIER));
    // A SET's data (the output buffer, METHOD_NEITHER): what the caller proposes or sets.
    if ((id->Flags & KSPROPERTY_TYPE_SET) && irp->UserBuffer && e->outLength) {
      const ULONG v = min(e->outLength, ULONG(sizeof(e->value)));
      __try {
        if (irp->RequestorMode == UserMode) ProbeForRead(irp->UserBuffer, v, 1);
        RtlCopyMemory(e->value, irp->UserBuffer, v);
        e->valueBytes = v;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
  }
  return true;
}

void LogRequest(WHAKsLogEntry* e, NTSTATUS status) {
  e->status = status;
  KIRQL irql;
  KeAcquireSpinLock(&g_logLock, &irql);
  e->sequence = g_logTotal;
  g_log[g_logTotal % WHA_KS_LOG_ENTRIES] = *e;
  ++g_logTotal;
  KeReleaseSpinLock(&g_logLock, irql);
}

NTSTATUS ReadLog(PIRP irp, PIO_STACK_LOCATION stack) {
  const ULONG outLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
  auto* out = static_cast<BYTE*>(irp->AssociatedIrp.SystemBuffer);
  if (!g_log) return STATUS_NOT_SUPPORTED;
  if (!out || outLength < sizeof(WHAKsLogHeader)) return STATUS_BUFFER_TOO_SMALL;
  const ULONG room = (outLength - sizeof(WHAKsLogHeader)) / sizeof(WHAKsLogEntry);
  auto* header = reinterpret_cast<WHAKsLogHeader*>(out);
  auto* entries = reinterpret_cast<WHAKsLogEntry*>(out + sizeof(WHAKsLogHeader));
  KIRQL irql;
  KeAcquireSpinLock(&g_logLock, &irql);
  const ULONG total = g_logTotal;
  const ULONG kept = min(total, WHA_KS_LOG_ENTRIES);
  const ULONG count = min(kept, room);
  for (ULONG i = 0; i < count; ++i) entries[i] = g_log[(total - count + i) % WHA_KS_LOG_ENTRIES];  // newest `count`
  KeReleaseSpinLock(&g_logLock, irql);
  header->total = total;
  header->entries = count;
  irp->IoStatus.Information = sizeof(WHAKsLogHeader) + count * sizeof(WHAKsLogEntry);
  return STATUS_SUCCESS;
}

NTSTATUS ControlDispatch(PIRP irp) {
  PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
  NTSTATUS status = STATUS_SUCCESS;
  irp->IoStatus.Information = 0;
  switch (stack->MajorFunction) {
    case IRP_MJ_CREATE:
    case IRP_MJ_CLEANUP:
    case IRP_MJ_CLOSE:
      break;
    case IRP_MJ_DEVICE_CONTROL:
      switch (stack->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_WHA_CABLE_EXCHANGE:
          status = Exchange(irp, stack);
          break;
        case IOCTL_WHA_KS_LOG:
          status = ReadLog(irp, stack);
          break;
        default:
          status = STATUS_INVALID_DEVICE_REQUEST;
          break;
      }
      break;
    default:
      status = STATUS_INVALID_DEVICE_REQUEST;
      break;
  }
  if (!NT_SUCCESS(status)) irp->IoStatus.Information = 0;
  irp->IoStatus.Status = status;
  IoCompleteRequest(irp, IO_NO_INCREMENT);
  return status;
}

// PortCls owns the dispatch table; IRPs for the control device are ours.
_Dispatch_type_(IRP_MJ_CREATE) _Dispatch_type_(IRP_MJ_CLEANUP) _Dispatch_type_(IRP_MJ_CLOSE)
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL) DRIVER_DISPATCH Dispatch;
NTSTATUS Dispatch(PDEVICE_OBJECT device, PIRP irp) {
  if (device == g_control) return ControlDispatch(irp);
  WHAKsLogEntry entry;
  const bool log = g_log && DescribeRequest(irp, &entry);
  const NTSTATUS status = PcDispatchIrp(device, irp);
  if (log) LogRequest(&entry, status);
  return status;
}

// The control device must go with the adapter device, or the driver never unloads.
_Dispatch_type_(IRP_MJ_PNP) DRIVER_DISPATCH PnpDispatch;
NTSTATUS PnpDispatch(PDEVICE_OBJECT device, PIRP irp) {
  const UCHAR minor = IoGetCurrentIrpStackLocation(irp)->MinorFunction;
  const NTSTATUS status = PcDispatchIrp(device, irp);
  if (device != g_control && minor == IRP_MN_REMOVE_DEVICE) DeleteControlDevice();
  return status;
}

DRIVER_ADD_DEVICE AddDevice;
NTSTATUS AddDevice(PDRIVER_OBJECT driver, PDEVICE_OBJECT physicalDevice) {
  NTSTATUS status = PcAddAdapterDevice(driver, physicalDevice, PCPFNSTARTDEVICE(StartDevice),
                                       kCables * kSubdevicesPerCable, 0);
  if (!NT_SUCCESS(status)) return status;
  // Without the control device the endpoints still work (the Worker falls back); report it only.
  const NTSTATUS control = CreateControlDevice(driver);
  if (!NT_SUCCESS(control)) DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "WinHookAudio: control device 0x%08X\n", control);
  return STATUS_SUCCESS;
}

DRIVER_UNLOAD DriverUnload;
void DriverUnload(PDRIVER_OBJECT driver) {
  DeleteControlDevice();
  if (g_portClsUnload) g_portClsUnload(driver);
  delete[] g_cables;
  g_cables = nullptr;
  delete[] g_log;
  g_log = nullptr;
}

}  // namespace
}  // namespace wha

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath) {
  using namespace wha;
  g_cables = new (POOL_FLAG_NON_PAGED, kPoolTag) WHACable[kCables]();
  if (!g_cables) return STATUS_INSUFFICIENT_RESOURCES;
  for (ULONG i = 0; i < kCables; ++i) {
    KeInitializeSpinLock(&g_cables[i].lock);
    SetCableFormatLocked(g_cables[i], WHACableFormat{});  // the data range PortCls reads at StartDevice
  }
  KeInitializeSpinLock(&g_logLock);
  g_log = new (POOL_FLAG_NON_PAGED, kPoolTag) WHAKsLogEntry[WHA_KS_LOG_ENTRIES];  // null: no log
  NTSTATUS status = PcInitializeAdapterDriver(driver, registryPath, PDRIVER_ADD_DEVICE(AddDevice));
  if (!NT_SUCCESS(status)) {
    delete[] g_cables;
    g_cables = nullptr;
    delete[] g_log;
    g_log = nullptr;
    return status;
  }
  driver->MajorFunction[IRP_MJ_CREATE] = Dispatch;
  driver->MajorFunction[IRP_MJ_CLEANUP] = Dispatch;
  driver->MajorFunction[IRP_MJ_CLOSE] = Dispatch;
  driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Dispatch;
  driver->MajorFunction[IRP_MJ_PNP] = PnpDispatch;
  g_portClsUnload = driver->DriverUnload;
  driver->DriverUnload = DriverUnload;
  return STATUS_SUCCESS;
}
