#include "MasterHolder.h"
#include "CableEndpointSync.h"
#include "KsAudio.h"
#include "KsCapture.h"
#include "HwOutputFifo.h"
#include "network/WHANetworkEngine.h"
#include "virtual/WHAIoctl.h"
#include "virtual/WHARingBuffer.h"
#include <winioctl.h>
#include "virtual/WHACableProtocol.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>

namespace wha {

MasterHolder::MasterHolder(WHASlotTable* table, float* masterAudio, WHABridgeShared* bridges[4],
                           HANDLE masterTick, HANDLE tableChanged, HANDLE bridgeTicks[4][4])
    : table_(table), masterAudio_(masterAudio), masterTick_(masterTick), tableChanged_(tableChanged) {
  for (int i = 0; i < 4; ++i) bridges_[i] = bridges[i];
  for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) bridgeTicks_[i][j] = bridgeTicks[i][j];
  workerDone_ = CreateEventA(nullptr, FALSE, TRUE, nullptr);  // signaled: the first tick has nothing to wait for
  for (int d = 0; d < kHwDevices; ++d) outRoute_[d] = inRoute_[d] = outRoutePub_[d] = inRoutePub_[d] = -1;
}
MasterHolder::~MasterHolder() {
  stop();
  if (workerDone_) CloseHandle(workerDone_);
  delete network_;
  for (int d = 0; d < kHwDevices; ++d) {
    delete out_[d];
    delete outFifo_[d];
    delete in_[d];
  }
  for (WHARingBuffer* ring : virtualRings_) delete ring;
}

bool MasterHolder::start() {
  if (running_) return true;
  // Preallocate virtual rings at start, not per-tick
  for (WHARingBuffer*& ring : virtualRings_) if (!ring) ring = new WHARingBuffer();
  cableIo_.resize(sizeof(WHACableExchange) + WHA_CABLE_MAX_FRAMES * WHA_CABLE_MAX_CHANNELS * sizeof(float));
  if (!network_) network_ = new WHANetworkEngine(table_);
  network_->start();  // socket opens only once a Network Stream is mapped
  stopRequested_ = false;
  running_ = true;
  DWORD id = 0;
  thread_ = CreateThread(nullptr, 0, threadProc, this, 0, &id);
  if (!thread_) { running_ = false; return false; }
  return true;
}
void MasterHolder::stop() {
  if (!running_) return;
  stopRequested_ = true;
  if (masterTick_) SetEvent(masterTick_);
  if (tableChanged_) SetEvent(tableChanged_);
  // Join without a timeout: the Worker's teardown (device Stop/Release, CoUninitialize) runs on this
  // object and in this DLL. Abandoning it after a timeout let it run on a deleted MasterHolder, whose
  // garbage state made an extra CoUninitialize tear down its apartment, and COM then unloaded the
  // DLL under the running thread (crash seen with a slow-closing capture device).
  if (thread_) {
    if (WaitForSingleObject(thread_, 1000) == WAIT_TIMEOUT) {
      OutputDebugStringA("WinHookAudio MasterHolder: Worker teardown > 1 s, still waiting\n");
      WaitForSingleObject(thread_, INFINITE);
    }
    CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (network_) network_->stop();
  running_ = false;
  if (workerDone_) SetEvent(workerDone_);
}
DWORD WINAPI MasterHolder::threadProc(LPVOID param) { static_cast<MasterHolder*>(param)->run(); return 0; }
void MasterHolder::run() {
  // MMCSS Pro Audio on worker thread
  avrtModule_ = LoadLibraryA("avrt.dll");
  if (avrtModule_) {
    using AvSetMmThreadCharacteristicsA = HANDLE(WINAPI*)(LPCSTR, LPDWORD);
    auto pfn = reinterpret_cast<AvSetMmThreadCharacteristicsA>(GetProcAddress(avrtModule_, "AvSetMmThreadCharacteristicsA"));
    if (pfn) { DWORD idx = 0; mmcssHandle_ = pfn("Pro Audio", &idx); }
  }
  const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));  // resolves endpoint IDs
  openHw();
  openCables();
  HANDLE handles[2] = {masterTick_, tableChanged_};
  int nHandles = (masterTick_ && tableChanged_) ? 2 : (masterTick_ ? 1 : 0);
  while (!stopRequested_) {
    DWORD wait = (nHandles > 0) ? WaitForMultipleObjects(nHandles, handles, FALSE, INFINITE) : WaitForSingleObject(masterTick_, INFINITE);
    if (stopRequested_) break;
    if (wait == WAIT_OBJECT_0 + 1) {  // TableChanged
      if (network_) network_->requestReconfigure();
      if (onTableChanged_) onTableChanged_();
    }
    // Route on Master_Tick only. A TableChanged-only wake used to run doTick too: an extra HW input
    // read / HW output write / Network block that no tick asked for. The next tick uses the new table.
    if (wait == WAIT_OBJECT_0) {
      if (stopRequested_) break;
      if (clockTicks_) {
        const uint64_t now = clockTicks_->load();
        if (haveTicks_ && now > ticksSeen_ + 1) {  // missed ticks: keep every HW device in step
          const size_t missed = static_cast<size_t>(now - ticksSeen_ - 1) * table_->general.asioBuffer;
          for (int d = 0; d < kHwDevices; ++d) {
            if (inRoute_[d] == d) in_[d]->skip(missed);
            if (d > 0 && outRoute_[d] == d) outFifo_[d]->skip(static_cast<int>(missed));
          }
        }
        ticksSeen_ = now;
        haveTicks_ = true;
      }
      doTick();
      routed_ = clockTicks_ ? ticksSeen_ : routed_.load() + 1;
      if (workerDone_) SetEvent(workerDone_);  // Master_Tick routed
    }
  }
  closeCables();
  closeHw();
  if (com) CoUninitialize();
  if (mmcssHandle_ && avrtModule_) {
    using AvRevertMmThreadCharacteristics = BOOL(WINAPI*)(HANDLE);
    auto pfn = reinterpret_cast<AvRevertMmThreadCharacteristics>(GetProcAddress(avrtModule_, "AvRevertMmThreadCharacteristics"));
    if (pfn) pfn(mmcssHandle_);
    FreeLibrary(avrtModule_);
    avrtModule_ = nullptr;
    mmcssHandle_ = nullptr;
  }
}
void MasterHolder::openHw() {
  // Which devices HW slots use; a device index with no ID (not in the list) cannot be opened.
  bool usedOut[kHwDevices] = {}, usedIn[kHwDevices] = {};
  bool anyHw = false;
  for (uint32_t i = 0; i < table_->masterOutCount; ++i)
    if (table_->masterOut[i].type == SLOT_HW) usedOut[HwDeviceOf(table_->masterOut[i])] = anyHw = true;
  for (uint32_t i = 0; i < table_->masterInCount; ++i)
    if (table_->masterIn[i].type == SLOT_HW) usedIn[HwDeviceOf(table_->masterIn[i])] = anyHw = true;
  usedOut[0] = anyHw;  // opened and started, output 0 is the Master Clock (WinHookMasterASIO::runClock)
  hwRequest_ = table_->general;  // one snapshot: what is opened is what the panel shows as requested
  CopyHwDeviceIds(*table_, hwRequestIds_);
  hwRequestValid_.store(true, std::memory_order_release);
  const WHAGeneral& g = hwRequest_;
  auto listed = [this](bool isInput, int d) { return d == 0 || HwDeviceId(hwRequestIds_, isInput, d)[0] != '\0'; };

  std::string resolved[kHwDevices];
  for (int d = 0; d < kHwDevices; ++d) {  // outputs
    const char* id = HwDeviceId(hwRequestIds_, false, d);
    if (!usedOut[d] || !listed(false, d)) continue;
    outUsed_[d] = true;
    resolved[d] = KsResolveEndpointId(eRender, id);
    for (int j = 0; j < d && outRoute_[d] < 0; ++j)
      if (!resolved[d].empty() && resolved[j] == resolved[d] && outRoute_[j] >= 0) outRoute_[d] = outRoute_[j];
    if (outRoute_[d] >= 0) continue;  // listed twice: plays through the earlier one
    if (!out_[d]) out_[d] = new KsAudio();
    if (out_[d]->open(g.sampleRate, g.hwBuffer, g.asioBuffer, id, HwDeviceMode(hwRequestIds_, false, d)) && out_[d]->start()) {
      const int channels = out_[d]->channels();
      if (d > 0) {
        if (!outFifo_[d]) outFifo_[d] = new HwOutputFifo();
        outFifo_[d]->reset(g.sampleRate, channels, static_cast<int>(g.asioBuffer), out_[d]->capacity());
        if (outFrames_.size() < static_cast<size_t>(out_[d]->capacity()) * channels)
          outFrames_.assign(static_cast<size_t>(out_[d]->capacity()) * channels, 0.0f);
      }
      hwOutBuf_[d].assign(static_cast<size_t>(channels) * 4096, 0.0f);
      outRoute_[d] = d;
      hwOut_[d] = out_[d];
    } else {
      hwOutError_[d] = FAILED(out_[d]->lastError()) ? static_cast<int32_t>(out_[d]->lastError()) : E_FAIL;  // open ok, Start failed
    }
  }
  for (int d = 0; d < kHwDevices; ++d) {  // inputs
    const char* id = HwDeviceId(hwRequestIds_, true, d);
    resolved[d].clear();
    if (!usedIn[d] || !listed(true, d)) continue;
    inUsed_[d] = true;
    resolved[d] = KsResolveEndpointId(eCapture, id);
    for (int j = 0; j < d && inRoute_[d] < 0; ++j)
      if (!resolved[d].empty() && resolved[j] == resolved[d] && inRoute_[j] >= 0) inRoute_[d] = inRoute_[j];
    if (inRoute_[d] >= 0) continue;  // listed twice: records through the earlier one
    if (!in_[d]) in_[d] = new KsCapture();
    if (in_[d]->open(g.sampleRate, g.hwBuffer, g.asioBuffer, id, HwDeviceMode(hwRequestIds_, true, d)) && in_[d]->start()) {
      hwInBuf_[d].assign(static_cast<size_t>(in_[d]->channels()) * 4096, 0.0f);
      inRoute_[d] = d;
      hwIn_[d] = in_[d];
    } else {
      hwInError_[d] = FAILED(in_[d]->lastError()) ? static_cast<int32_t>(in_[d]->lastError()) : E_FAIL;
    }
  }
  outTrace_.clear();
  if (GetEnvironmentVariableA("WINHOOKAUDIO_OUT_TRACE", outTracePath_, sizeof(outTracePath_)) > 0) outTrace_.reserve(1000000);
  for (int d = 0; d < kHwDevices; ++d) {
    outRoutePub_[d] = outRoute_[d];
    inRoutePub_[d] = inRoute_[d];
  }
}

void MasterHolder::closeHw() {
  for (int d = 0; d < kHwDevices; ++d) {  // the Master Clock is already stopped; never hand out a closing device
    hwOut_[d] = nullptr;
    hwIn_[d] = nullptr;
    outRoutePub_[d] = inRoutePub_[d] = -1;
    outRoute_[d] = inRoute_[d] = -1;
  }
  for (int d = 0; d < kHwDevices; ++d) {
    if (out_[d]) { out_[d]->stop(); out_[d]->close(); }
    if (in_[d]) { in_[d]->stop(); in_[d]->close(); }
  }
  if (!outTrace_.empty()) {
    FILE* f = nullptr;
    if (fopen_s(&f, outTracePath_, "w") == 0 && f) {
      std::fprintf(f, "sec,written,padding,wrote,backlog,ratio\n");
      for (const OutTraceRow& r : outTrace_)
        std::fprintf(f, "%.6f,%lld,%d,%d,%d,%.9f\n", r.sec, static_cast<long long>(r.written), r.padding, r.wrote, r.backlog, r.ratio);
      std::fclose(f);
    }
    outTrace_.clear();
  }
}

void MasterHolder::openCables() {
  cableCount_ = 0;
  cableCountPub_ = 0;
  for (CableFormatSent& sent : cableSent_) sent = CableFormatSent{};  // send every cable's format again
  HANDLE h = CreateFileW(WHA_CABLE_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    cableError_ = static_cast<int>(GetLastError());  // 2: not installed; 5: another Worker has it
    return;
  }
  WHACableExchange probe{};
  probe.protocol = WHA_CABLE_PROTOCOL;
  DWORD got = 0;
  if (!DeviceIoControl(h, IOCTL_WHA_CABLE_EXCHANGE, &probe, sizeof(probe), &probe, sizeof(probe), &got, nullptr) ||
      got != sizeof(probe) || probe.protocol != WHA_CABLE_PROTOCOL) {
    cableError_ = static_cast<int>(ERROR_REVISION_MISMATCH);
    CloseHandle(h);
    return;
  }
  cableDevice_ = h;
  cableCount_ = static_cast<int>(probe.cables < kVirtualSlotCables ? probe.cables : kVirtualSlotCables);
  cableError_ = 0;
  cableCountPub_ = cableCount_;
  for (CableStat& st : cableStat_) st.rate = 0;  // no accepted format until this run's first exchange
  // Windows keeps an endpoint's device format: have the cable endpoints follow what the driver accepts.
  endpointSync_ = new CableEndpointSync([this](int c, uint32_t& rate, uint32_t& channels, uint32_t& format) {
    if (c < 0 || c >= cableCountPub_.load()) return false;
    const CableStat& st = cableStat_[c];
    rate = st.rate.load();
    channels = st.channels.load();
    format = st.format.load();
    return rate != 0;
  });
  endpointSync_->start();
}
void MasterHolder::closeCables() {
  delete endpointSync_;  // joins its thread
  endpointSync_ = nullptr;
  cableCountPub_ = 0;
  cableCount_ = 0;
  if (cableDevice_ != INVALID_HANDLE_VALUE) CloseHandle(cableDevice_);
  cableDevice_ = INVALID_HANDLE_VALUE;
}
bool MasterHolder::exchangeCable(int cable, uint32_t frames, bool hasRecord, uint32_t channels, uint32_t format) {
  CableStat& st = cableStat_[cable];
  auto* h = reinterpret_cast<WHACableExchange*>(cableIo_.data());
  std::memset(h, 0, sizeof(*h));
  h->protocol = WHA_CABLE_PROTOCOL;
  h->cable = static_cast<unsigned>(cable);
  h->frames = frames;
  h->hasRecord = hasRecord ? 1u : 0u;
  h->rate = table_->general.sampleRate;  // the cable's endpoints run at the Master Clock's rate
  h->channels = channels;
  h->format = format;
  const size_t audioBytes = static_cast<size_t>(frames) * channels * sizeof(float);
  const DWORD bytes = static_cast<DWORD>(sizeof(WHACableExchange) + audioBytes);
  if (hasRecord) std::memcpy(cableIo_.data() + sizeof(WHACableExchange), virtualScratch_, audioBytes);
  DWORD got = 0;
  st.exchanges.fetch_add(1, std::memory_order_relaxed);
  if (!DeviceIoControl(cableDevice_, IOCTL_WHA_CABLE_EXCHANGE, cableIo_.data(), hasRecord ? bytes : sizeof(WHACableExchange),
                       cableIo_.data(), bytes, &got, nullptr) || got != bytes) {
    st.errors.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const bool formatChanged = st.rate.load() != h->rate || st.channels.load() != h->channels || st.format.load() != h->format;
  cableSent_[cable] = CableFormatSent{h->rate, h->channels, h->format};
  st.rate = h->rate;
  st.channels = h->channels;
  st.format = h->format;
  st.playRate = h->playRate;
  st.recordRate = h->recordRate;
  st.playFill = h->playFill;
  st.recordFill = h->recordFill;
  st.playUnderruns = h->playUnderruns;
  st.playDrops = h->playDrops;
  st.recordUnderruns = h->recordUnderruns;
  st.recordDrops = h->recordDrops;
  if (formatChanged && endpointSync_) endpointSync_->wake();  // after st.* hold the new format
  return true;
}
bool MasterHolder::cableStatus(int c, WHACableExchange& reply, uint64_t& exchanges, uint64_t& errors) const {
  if (c < 0 || c >= kVirtualSlotCables) return false;
  const CableStat& st = cableStat_[c];
  exchanges = st.exchanges.load();
  errors = st.errors.load();
  if (!exchanges) return false;
  reply = WHACableExchange{};
  reply.protocol = WHA_CABLE_PROTOCOL;
  reply.cable = static_cast<unsigned>(c);
  reply.rate = st.rate;
  reply.channels = st.channels;
  reply.format = st.format;
  reply.playRate = st.playRate;
  reply.recordRate = st.recordRate;
  reply.playFill = st.playFill;
  reply.recordFill = st.recordFill;
  reply.playUnderruns = st.playUnderruns;
  reply.playDrops = st.playDrops;
  reply.recordUnderruns = st.recordUnderruns;
  reply.recordDrops = st.recordDrops;
  return true;
}
void MasterHolder::tickOnce() {
  for (WHARingBuffer*& ring : virtualRings_) if (!ring) ring = new WHARingBuffer();
  doTick();
}

void MasterHolder::doTick() {
  if (!table_ || !masterAudio_) return;
  // Per-thing FIFOs: for now, just handle Loopback OUT->IN next tick and Bridge sum
  // Loopback: if masterOut[i].loopback && type==VIRTUAL, copy OUT->IN next tick
  // Master audio is float[2][512][4096] ping-pong; for 09 offline, simulate with masterAudio_ as flat
  // For offline test, masterAudio_ is 16MB = 2*512*4096*4; use active buffer 0
  // Loopback: copy OUT slot's audio to paired IN slot's audio (same Virtual index)
  // Loopback: only OUT loopback matters, find IN by srcChannel (IN loopback not required)
  for (uint32_t oi = 0; oi < table_->masterOutCount; ++oi) {
    const auto& outSlot = table_->masterOut[oi];
    if (outSlot.loopback && outSlot.type == SLOT_VIRTUAL) {
      for (uint32_t ii = 0; ii < table_->masterInCount; ++ii) {
        const auto& inSlot = table_->masterIn[ii];
        if (inSlot.type == SLOT_VIRTUAL && inSlot.srcChannel == outSlot.srcChannel) {
          int frames = static_cast<int>(table_->general.asioBuffer);
          if (frames > 4096) frames = 4096;
          // Flat layout for 09 offline: OUT at oi*4096, IN at 512*4096+ii*4096
          float* outBuf = masterAudio_ + oi * 4096;
          float* inBuf = masterAudio_ + 512 * 4096 + ii * 4096;
          std::memcpy(inBuf, outBuf, frames * sizeof(float));
          break;
        }
      }
    }
  }
  // Shared Bridge: numbered blocks (see WHABridgeShared). This tick is block t of each Bridge: the
  // Bridge's Master OUT slots go to its clients as block t; its Master IN slots get the clients'
  // block t - delay, summed (tanh), silence from a client that has not produced it yet (counted late).
  for (int bi = 0; bi < 4; ++bi) {
    auto* b = bridges_[bi];
    if (!b) continue;
    int frames = static_cast<int>(table_->general.asioBuffer);
    if (frames > static_cast<int>(kBridgeFrames)) frames = static_cast<int>(kBridgeFrames);
    if (frames <= 0) frames = 128;
    const int64_t t = b->masterBlocks;
    const WHASlotType want = static_cast<WHASlotType>(SLOT_BRIDGE1 + bi);
    // Master OUT -> block t for every client. Each BRIDGE(bi) OUT slot picks its channel
    // (BridgeChannelOf); two slots on one channel are summed. A channel of this ring slot that was fed
    // when the slot was last used, and is not now, is cleared.
    const uint32_t slot = static_cast<uint32_t>(t % kBridgeRing);
    float (*to)[kBridgeFrames] = b->toClients[slot];
    uint64_t fed = 0;
    for (uint32_t oi = 0; oi < table_->masterOutCount; ++oi) {
      const int ch = table_->masterOut[oi].type == want ? BridgeChannelOf(table_->masterOut[oi]) : -1;
      if (ch < 0) continue;
      const float* src = masterAudio_ + oi * 4096;
      if (fed & (1ull << ch)) {
        for (int f = 0; f < frames; ++f) to[ch][f] += src[f];
      } else {
        std::memcpy(to[ch], src, frames * sizeof(float));
        fed |= 1ull << ch;
      }
    }
    for (uint64_t stale = bridgeFed_[bi][slot] & ~fed; stale; stale &= stale - 1) {
      const int ch = std::countr_zero(stale);
      std::memset(to[ch], 0, kBridgeFrames * sizeof(float));
    }
    bridgeFed_[bi][slot] = fed;
    // Clients' block t - delay -> Master IN slots.
    const int64_t mixBlock = t - BridgeDelayBlocks(table_->general.bridgeBuffer[bi], table_->general.asioBuffer);
    bool have[kBridgeClients] = {};
    for (uint32_t ci = 0; ci < kBridgeClients; ++ci) {
      const int64_t produced = b->clientBlocks[ci];
      if (b->owner[ci] == 0 || produced < 0 || mixBlock < 0) continue;  // no app, not running, or too early
      have[ci] = produced > mixBlock;
      if (!have[ci]) b->clientLate[ci] = b->clientLate[ci] + 1;
    }
    const uint32_t mixSlot = static_cast<uint32_t>((mixBlock < 0 ? 0 : mixBlock) % kBridgeRing);
    for (uint32_t ii = 0; ii < table_->masterInCount; ++ii) {
      const int ch = table_->masterIn[ii].type == want ? BridgeChannelOf(table_->masterIn[ii]) : -1;
      if (ch < 0) continue;
      float* dst = masterAudio_ + (512 + ii) * 4096;
      std::memset(dst, 0, frames * sizeof(float));
      for (uint32_t ci = 0; ci < kBridgeClients; ++ci)
        if (have[ci])
          for (int f = 0; f < frames; ++f) dst[f] += b->fromClient[ci][mixSlot][ch][f];
      for (int f = 0; f < frames; ++f) dst[f] = std::tanh(dst[f]);
    }
    // Publish block t (after its audio), then wake every client.
    InterlockedExchange64(&b->masterBlocks, t + 1);
    for (int ci = 0; ci < 4; ++ci)
      if (bridgeTicks_[bi][ci]) SetEvent(bridgeTicks_[bi][ci]);
  }
  // Virtual Cable. A VIRTUAL slot's source is one channel of one cable (VirtualCableOf/VirtualChannelOf);
  // the cable has CableSetting(*table_, c).channels of them (a slot on a channel past that is silent). Per
  // cable, OUT slots are summed into their channel; one block feeds every IN slot of it. A cable
  // WinHookAudio.sys has goes to and from Windows: OUT -> its recording endpoint, its playback
  // endpoint -> IN (silence while nothing plays or an exchange fails); each exchange also sends the
  // cable's format, and an idle cable gets it once when it changes. Other cables loop OUT -> IN
  // inside the Worker (the ring stub, always 8 channels wide). One pass over the slots finds the cables
  // in use, so a cable without slots costs nothing (the slot lists are scanned per cable only for those).
  {
    const uint32_t frames = table_->general.asioBuffer < 4096 ? table_->general.asioBuffer : 4096;
    uint32_t outCables = 0, inCables = 0;  // bit c: cable c has a VIRTUAL OUT / IN slot
    for (uint32_t oi = 0; oi < table_->masterOutCount; ++oi)
      if (table_->masterOut[oi].type == SLOT_VIRTUAL) outCables |= 1u << VirtualCableOf(table_->masterOut[oi]);
    for (uint32_t ii = 0; ii < table_->masterInCount; ++ii)
      if (table_->masterIn[ii].type == SLOT_VIRTUAL) inCables |= 1u << VirtualCableOf(table_->masterIn[ii]);
    static_assert(kVirtualSlotCables <= 32, "one bit per cable");
    for (int cable = 0; cable < static_cast<int>(kVirtualSlotCables); ++cable) {
      if (!virtualRings_[cable] || !frames) continue;
      const WHACableSetting setting = CableSetting(*table_, cable);
      const bool driverCable = cable < cableCount_;
      const uint32_t cableChannels = IsValidCableSetting(setting) ? setting.channels : 2u;
      const uint32_t format = IsValidCableSetting(setting) ? setting.format : 0u;
      if (!((outCables | inCables) >> cable & 1u)) {  // idle: a driver cable still gets a changed format
        if (driverCable) {
          const CableFormatSent& sent = cableSent_[cable];
          if (sent.rate != table_->general.sampleRate || sent.channels != cableChannels || sent.format != format)
            exchangeCable(cable, 0, false, cableChannels, format);
        }
        continue;
      }
      const uint32_t width = driverCable ? cableChannels : kVirtualChannels;  // interleave of virtualScratch_
      bool anyOut = false, anyIn = false;
      std::memset(virtualScratch_, 0, sizeof(float) * width * frames);
      for (uint32_t oi = 0; (outCables >> cable & 1u) && oi < table_->masterOutCount; ++oi) {
        const WHASlot& slot = table_->masterOut[oi];
        if (slot.type != SLOT_VIRTUAL || VirtualCableOf(slot) != cable) continue;
        const uint32_t ch = static_cast<uint32_t>(VirtualChannelOf(slot));
        if (ch >= cableChannels) continue;
        const float* outBuf = masterAudio_ + oi * 4096;
        for (uint32_t f = 0; f < frames; ++f) virtualScratch_[f * width + ch] += outBuf[f];
        anyOut = true;
      }
      for (uint32_t ii = 0; (inCables >> cable & 1u) && ii < table_->masterInCount; ++ii) {
        const WHASlot& slot = table_->masterIn[ii];
        if (slot.type != SLOT_VIRTUAL || VirtualCableOf(slot) != cable) continue;
        anyIn = true;
        if (static_cast<uint32_t>(VirtualChannelOf(slot)) >= cableChannels)  // past the cable's channels: silent
          std::memset(masterAudio_ + 512 * 4096 + ii * 4096, 0, sizeof(float) * frames);
      }
      const float* in = virtualScratch_;  // interleaved, `width` wide, for the cable's IN slots
      if (driverCable) {
        if (!anyOut && !anyIn) {
          const CableFormatSent& sent = cableSent_[cable];
          if (sent.rate != table_->general.sampleRate || sent.channels != cableChannels || sent.format != format)
            exchangeCable(cable, 0, false, cableChannels, format);
          continue;
        }
        if (exchangeCable(cable, frames, anyOut, cableChannels, format))
          in = reinterpret_cast<const float*>(cableIo_.data() + sizeof(WHACableExchange));
        else
          std::memset(virtualScratch_, 0, sizeof(float) * width * frames);
        if (!anyIn) continue;
      } else {
        if (anyOut) virtualRings_[cable]->write(virtualScratch_, frames);
        if (!anyIn || virtualRings_[cable]->size() < frames || !virtualRings_[cable]->read(virtualScratch_, frames)) continue;
      }
      for (uint32_t ii = 0; ii < table_->masterInCount; ++ii) {
        const WHASlot& slot = table_->masterIn[ii];
        if (slot.type != SLOT_VIRTUAL || VirtualCableOf(slot) != cable) continue;
        float* inBuf = masterAudio_ + 512 * 4096 + ii * 4096;
        const uint32_t ch = static_cast<uint32_t>(VirtualChannelOf(slot));
        if (ch >= cableChannels) continue;  // cleared above
        for (uint32_t f = 0; f < frames; ++f) inBuf[f] = in[f * width + ch];
      }
    }
  }
  // Network Streams: OUT slots -> Tx rings, Rx rings (jitter-primed) -> IN slots
  if (network_) {
    uint32_t frames = table_->general.asioBuffer;
    if (frames > 4096) frames = 4096;
    network_->processTick(masterAudio_, masterAudio_ + 512 * 4096, 4096, frames);
  }
  int hwFrames = static_cast<int>(table_->general.asioBuffer);
  if (hwFrames > 4096) hwFrames = 4096;
  // Master Clock settling (setClockSettled): the HW inputs and more outputs wait in silence, then start
  // afresh once it has.
  const bool settled = !clockSettled_ || clockSettled_->load();
  if (settled && hwHeld_) {
    for (int d = 0; d < kHwDevices; ++d) {
      if (inRoute_[d] == d) in_[d]->restart();
      if (d > 0 && outRoute_[d] == d) {
        outFifo_[d]->reset(hwRequest_.sampleRate, out_[d]->channels(), static_cast<int>(hwRequest_.asioBuffer), out_[d]->capacity());
        outFifo_[d]->primeSilence(static_cast<int32_t>(out_[d]->padding()));
      }
    }
  }
  hwHeld_ = !settled;
  // KS write: every HW OUT slot summed into its device's channel (srcChannel 0 = L, 1 = R, 2 = Ch 3
  // ...; one past what the device opened is dropped). Output 0 is the Master Clock's device, written
  // directly; the others through their FIFO (own clocks).
  bool anyOut = false;
  for (int d = 0; d < kHwDevices; ++d) {
    if (outRoute_[d] != d) continue;
    std::memset(hwOutBuf_[d].data(), 0, sizeof(float) * out_[d]->channels() * static_cast<size_t>(hwFrames));
    anyOut = true;
  }
  if (anyOut) {
    for (uint32_t oi = 0; oi < table_->masterOutCount; ++oi) {
      const auto& slot = table_->masterOut[oi];
      if (slot.type != SLOT_HW || slot.srcChannel < 0) continue;
      const int p = outRoute_[HwDeviceOf(slot)];
      if (p < 0 || slot.srcChannel >= out_[p]->channels()) continue;
      const float* outBuf = masterAudio_ + oi * 4096;
      float* dst = hwOutBuf_[p].data() + static_cast<size_t>(slot.srcChannel) * hwFrames;
      for (int f = 0; f < hwFrames; ++f) dst[f] += outBuf[f];
    }
  }
  if (outRoute_[0] == 0) out_[0]->write(hwOutBuf_[0].data(), hwFrames, out_[0]->channels());
  for (int d = 1; d < kHwDevices; ++d) {
    if (outRoute_[d] != d) continue;
    const int channels = out_[d]->channels();
    if (!settled) {  // silence at the device's target fill until the Master Clock settles
      const long padding = out_[d]->padding();
      const long n = padding < 0 ? 0 : out_[d]->targetFill() - padding;
      if (n > 0) {
        std::memset(outFrames_.data(), 0, sizeof(float) * static_cast<size_t>(n) * channels);
        out_[d]->writeInterleaved(outFrames_.data(), static_cast<int>(n), channels);
      }
      continue;
    }
    outFifo_[d]->push(hwOutBuf_[d].data(), hwFrames, channels);
    int64_t written = 0;
    const long padding = out_[d]->padding(&written);
    if (padding < 0) continue;  // device gone
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double nowSec = static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
    const int n = outFifo_[d]->plan(nowSec, written, static_cast<int32_t>(padding));
    if (d == 1 && outTrace_.capacity() && outTrace_.size() < outTrace_.capacity())
      outTrace_.push_back({nowSec, written, static_cast<int32_t>(padding), n, outFifo_[d]->fill(), outFifo_[d]->ratio()});
    if (n <= 0) continue;
    outFifo_[d]->pop(outFrames_.data(), n);
    out_[d]->writeInterleaved(outFrames_.data(), n, channels);
  }
  // KS read: each open HW input once, then each HW IN slot from its device's channel; the DAW gets it
  // next tick. A slot whose device is not open is silent.
  bool anyIn = false;
  for (int d = 0; d < kHwDevices; ++d) {
    if (inRoute_[d] != d) continue;
    if (settled) in_[d]->read(hwInBuf_[d].data(), hwFrames, in_[d]->channels());
    else in_[d]->hold(hwInBuf_[d].data(), hwFrames, in_[d]->channels());
    anyIn = true;
  }
  if (!anyIn) return;
  for (uint32_t ii = 0; ii < table_->masterInCount; ++ii) {
    const auto& slot = table_->masterIn[ii];
    if (slot.type != SLOT_HW) continue;
    float* inBuf = masterAudio_ + 512 * 4096 + ii * 4096;
    const int p = inRoute_[HwDeviceOf(slot)];
    if (p >= 0 && slot.srcChannel >= 0 && slot.srcChannel < in_[p]->channels())
      std::memcpy(inBuf, hwInBuf_[p].data() + static_cast<size_t>(slot.srcChannel) * hwFrames, sizeof(float) * hwFrames);
    else
      std::memset(inBuf, 0, sizeof(float) * hwFrames);
  }
}

}  // namespace wha
