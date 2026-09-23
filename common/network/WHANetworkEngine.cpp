#include "WHANetworkEngine.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "WHANetwork.h"
#include "WHADriftResampler.h"
#include "WHAPacket.h"
#include "WHASpscRing.h"

#pragma comment(lib, "ws2_32.lib")

namespace wha {

namespace {

constexpr uint32_t kMaxTickFrames = 4096;        // Master audio slot stride
constexpr uint32_t kVorbisFeedFrames = 1024;     // encoder input chunk
constexpr uint64_t kCodebookRepeatMs = 2000;     // late joiners get the CODEBOOK within 2 s
constexpr int32_t kSeqResync = 1024;            // ~2.7 s of 128-frame packets

// Clock drift (sender crystal vs Master Clock). Fill is smoothed over kFillTauSec; the baseline is
// taken kBaselineSec after priming; correction engages only once fill leaves the deadband, so equal
// clocks never resample. PI gains are per frame of error, normalised by the sample rate.
constexpr double kFillTauSec = 1.0;
constexpr double kBaselineSec = 2.0;
constexpr double kDriftKp = 0.5;     // x 1/rate per frame of error
constexpr double kDriftKi = 0.25;    // x 1/rate per frame*second
constexpr double kMaxDriftPpm = 2500.0;

uint64_t Qpc() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return static_cast<uint64_t>(v.QuadPart);
}
uint64_t QpcFreq() {
  LARGE_INTEGER v;
  QueryPerformanceFrequency(&v);
  return static_cast<uint64_t>(v.QuadPart);
}

bool ParseIp(const char* ip, in_addr* out) {
  if (!ip || !ip[0]) return false;
  char buf[sizeof(WHANetworkStream::ip) + 1] = {};
  std::memcpy(buf, ip, sizeof(WHANetworkStream::ip));
  return inet_pton(AF_INET, buf, out) == 1;
}

bool StreamMapped(const WHASlot* slots, uint32_t count, uint32_t stream) {
  for (uint32_t i = 0; i < count; ++i)
    if (slots[i].type == SLOT_NETWORK && slots[i].streamId == static_cast<int32_t>(stream)) return true;
  return false;
}

// Worker/network handshake: the Worker marks itself busy while touching a stream;
// the network thread only reallocates a stream after clearing `active` and seeing busy == 0.
struct StreamGate {
  std::atomic<bool> active{false};
  std::atomic<uint32_t> busy{0};

  bool enter() {
    if (!active.load()) return false;
    busy.store(1);
    if (!active.load()) {
      busy.store(0);
      return false;
    }
    return true;
  }
  void leave() { busy.store(0); }
  void close() {
    active.store(false);
    while (busy.load()) YieldProcessor();
  }
};

}  // namespace

struct WHANetworkEngine::Impl {
  struct Tx {
    StreamGate gate;
    WHASpscRing ring;
    std::vector<float> gather;  // Worker scratch
    WHANetworkCounters c;
    // network thread only
    WHANetworkStream cfg{};
    uint32_t rate = 0;
    sockaddr_in dest{};
    uint32_t sequence = 0;
    uint32_t packetFrames = 0;
    std::vector<float> block;
    WHAVorbisEncoder enc;
    std::vector<uint8_t> codebookPacket;
    uint64_t lastCodebookQpc = 0;
    std::vector<std::vector<uint8_t>> vorbisPackets;
  };
  struct Rx {
    StreamGate gate;
    WHASpscRing ring;
    std::vector<float> scratch;  // Worker scratch
    std::atomic<bool> primed{false};
    std::atomic<uint32_t> target{0};
    WHANetworkCounters c;
    // network thread only
    WHANetworkStream cfg{};
    uint32_t rate = 0;
    bool peerFilter = false;
    in_addr peer{};
    bool haveSeq = false;
    uint32_t nextSeq = 0;
    WHAVorbisDecoder dec;
    bool decoderOpen = false;
    std::vector<uint8_t> codebookHeaders;
    WHAResampler src;
    bool resample = false;
    std::vector<float> pcm, decoded, resampled, drifted;
    // drift compensation (network thread)
    WHADriftResampler drift;
    double ratio = 1.0;
    double fillEma = 0.0;
    double baseline = 0.0;
    bool baselineSet = false;
    bool wasPrimed = false;
    uint64_t primedQpc = 0;
    uint64_t lastQpc = 0;
    double integ = 0.0;
    std::atomic<bool> driftEngaged{false};
    std::atomic<int32_t> driftPpmMilli{0};  // estimated sender drift, ppm x 1000
  };

  WHANetworkEngine* owner = nullptr;
  const WHASlotTable* table = nullptr;
  WHANetworkOptions options;
  Tx tx[kNetStreams];
  Rx rx[kNetStreams];
  HANDLE thread = nullptr;
  HANDLE wake = nullptr;
  WSAEVENT sockEvent = WSA_INVALID_EVENT;
  SOCKET sock = INVALID_SOCKET;
  bool wsaStarted = false;
  std::atomic<bool> quit{false};
  std::atomic<bool> reconfigure{true};
  mutable std::mutex errorMutex;
  std::string socketError;
  std::vector<uint8_t> recvBuf = std::vector<uint8_t>(65536);
  std::vector<uint8_t> datagram;
  uint64_t qpcFreq = QpcFreq();

  static DWORD WINAPI ThreadProc(void* self) {
    static_cast<Impl*>(self)->Run();
    return 0;
  }

  void SetSocketError(const std::string& e) {
    std::lock_guard<std::mutex> lock(errorMutex);
    socketError = e;
  }

  void Run() {
    WSADATA wsa;
    wsaStarted = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    if (!wsaStarted) SetSocketError("WSAStartup failed");
    sockEvent = WSACreateEvent();
    while (!quit.load()) {
      if (reconfigure.exchange(false)) {
        ApplyConfig();
        owner->generation_.fetch_add(1);
      }
      HANDLE waits[2] = {wake, sockEvent};
      const DWORD n = sock != INVALID_SOCKET ? 2 : 1;
      WaitForMultipleObjects(n, waits, FALSE, 50);  // Worker Tx and socket Rx both signal; timeout is a backstop
      if (sock != INVALID_SOCKET) WSAResetEvent(sockEvent);
      for (uint32_t i = 0; i < kNetStreams; ++i) ServiceTx(i);
      ServiceRx();
    }
    for (uint32_t i = 0; i < kNetStreams; ++i) {
      CloseTx(tx[i]);
      CloseRx(rx[i]);
    }
    CloseSocket();
    if (sockEvent != WSA_INVALID_EVENT) WSACloseEvent(sockEvent);
    sockEvent = WSA_INVALID_EVENT;
    if (wsaStarted) WSACleanup();
  }

  // ---- configuration (network thread) ----

  void CloseTx(Tx& s) {
    s.gate.close();
    s.enc.Close();
  }
  void CloseRx(Rx& s) {
    s.gate.close();
    s.dec.Close();
    s.decoderOpen = false;
    s.codebookHeaders.clear();
    s.src.Close();
    s.resample = false;
    s.haveSeq = false;
    s.primed.store(false);
    ResetDrift(s);
  }
  void ResetDrift(Rx& s) {
    s.drift.reset(s.cfg.channels ? s.cfg.channels : 1);
    s.ratio = 1.0;
    s.fillEma = 0.0;
    s.baselineSet = false;
    s.wasPrimed = false;
    s.lastQpc = 0;
    s.integ = 0.0;
    s.driftEngaged.store(false);
    s.driftPpmMilli.store(0);
  }

  uint32_t RingFrames(uint32_t rate) const { return rate * options.ringMs / 1000 + kMaxTickFrames; }

  void ApplyConfig() {
    const uint32_t rate = table->general.sampleRate;
    bool anyActive = false;
    for (uint32_t i = 0; i < kNetStreams; ++i) {
      Tx& s = tx[i];
      const WHANetworkStream cfg = table->netTx[i];
      in_addr addr{};
      const bool want = StreamMapped(table->masterOut, table->masterOutCount, i) && ParseIp(cfg.ip, &addr) &&
                        cfg.port != 0 && IsValidCodec(cfg.codec) && IsValidNetworkChannels(cfg.codec, cfg.channels);
      const bool same = s.gate.active.load() == want && s.rate == rate && std::memcmp(&s.cfg, &cfg, sizeof(cfg)) == 0;
      if (!same) {
        CloseTx(s);
        s.cfg = cfg;
        s.rate = rate;
        if (want && OpenTx(i, s, addr)) s.gate.active.store(true);
      }
      anyActive |= s.gate.active.load();
    }
    for (uint32_t i = 0; i < kNetStreams; ++i) {
      Rx& s = rx[i];
      const WHANetworkStream cfg = table->netRx[i];
      const bool want = StreamMapped(table->masterIn, table->masterInCount, i) && IsValidCodec(cfg.codec) &&
                        IsValidNetworkChannels(cfg.codec, cfg.channels);
      const uint32_t jitterMs = cfg.codec == WHA_VORBIS ? table->general.jitterVorbis : table->general.jitterPcm;
      const uint32_t target = rate * jitterMs / 1000;
      const bool same = s.gate.active.load() == want && s.rate == rate && s.target.load() == target &&
                        std::memcmp(&s.cfg, &cfg, sizeof(cfg)) == 0;
      if (!same) {
        CloseRx(s);
        s.cfg = cfg;
        s.rate = rate;
        s.target.store(target);
        s.peerFilter = ParseIp(cfg.ip, &s.peer);
        if (want) {
          s.ring.allocate(cfg.channels, RingFrames(rate));
          s.drift.reset(cfg.channels);
          s.scratch.assign(static_cast<size_t>(kMaxTickFrames) * cfg.channels, 0.0f);
          s.gate.active.store(true);
        }
      }
      anyActive |= s.gate.active.load();
    }
    if (anyActive && sock == INVALID_SOCKET) OpenSocket();
  }

  bool OpenTx(uint32_t i, Tx& s, const in_addr& addr) {
    const uint32_t ch = s.cfg.channels;
    s.ring.allocate(ch, RingFrames(s.rate));
    s.gather.assign(static_cast<size_t>(kMaxTickFrames) * ch, 0.0f);
    s.dest = sockaddr_in{};
    s.dest.sin_family = AF_INET;
    s.dest.sin_port = htons(s.cfg.port);
    s.dest.sin_addr = addr;
    s.sequence = 0;
    s.codebookPacket.clear();
    s.lastCodebookQpc = 0;
    const uint32_t bytesPerSample = s.cfg.codec == WHA_PCM_I16 ? 2 : 4;
    uint32_t framesFit = kWhaaMaxPayload / (ch * bytesPerSample);
    uint32_t pf = table->general.asioBuffer;
    if (pf > framesFit) pf = framesFit;
    s.packetFrames = pf ? pf : 1;
    s.block.assign(static_cast<size_t>(kVorbisFeedFrames > s.packetFrames ? kVorbisFeedFrames : s.packetFrames) * ch, 0.0f);
    if (s.cfg.codec == WHA_VORBIS) {
      WHACodebook cb;
      if (!s.enc.Open(i, s.rate, ch, s.cfg.quality, cb)) {
        s.c.errors.fetch_add(1);
        return false;
      }
      WHAACodebookHeader h{};
      h.streamId = i;
      h.sampleRate = s.rate;
      h.channels = ch;
      h.quality = s.cfg.quality;
      h.headersBytes = static_cast<uint32_t>(cb.headers.size());
      s.codebookPacket.resize(sizeof(h) + cb.headers.size());
      std::memcpy(s.codebookPacket.data(), &h, sizeof(h));
      std::memcpy(s.codebookPacket.data() + sizeof(h), cb.headers.data(), cb.headers.size());
    }
    return true;
  }

  void OpenSocket() {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
      SetSocketError("socket failed " + std::to_string(WSAGetLastError()));
      return;
    }
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(options.rxPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    // Tx still works on an ephemeral port when the Rx port is taken; Rx then receives nothing.
    if (bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
      SetSocketError("bind UDP " + std::to_string(options.rxPort) + " failed " + std::to_string(WSAGetLastError()));
    WSAEventSelect(sock, sockEvent, FD_READ);  // also makes the socket non-blocking
  }
  void CloseSocket() {
    if (sock != INVALID_SOCKET) closesocket(sock);
    sock = INVALID_SOCKET;
  }

  // ---- Tx (network thread) ----

  void Send(Tx& s, const void* head, size_t headBytes, const uint8_t* payload, size_t payloadBytes) {
    datagram.resize(headBytes + payloadBytes);
    std::memcpy(datagram.data(), head, headBytes);
    if (payloadBytes) std::memcpy(datagram.data() + headBytes, payload, payloadBytes);
    const int sent = sendto(sock, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
                            reinterpret_cast<const sockaddr*>(&s.dest), sizeof(s.dest));
    if (sent != static_cast<int>(datagram.size())) {
      s.c.errors.fetch_add(1);
      return;
    }
    s.c.packets.fetch_add(1);
    s.c.bytes.fetch_add(datagram.size());
  }

  void SendAudio(uint32_t i, Tx& s, uint32_t frames, const uint8_t* payload, size_t bytes) {
    WHAAPacketHeader h{};
    h.codec = s.cfg.codec;
    h.channels = s.cfg.channels;
    h.frames = frames < 1 ? 1 : (frames > 4096 ? 4096 : frames);
    h.streamId = i;
    h.sequence = s.sequence++;
    h.qpc = Qpc();
    h.payloadBytes = static_cast<uint32_t>(bytes);
    Send(s, &h, sizeof(h), payload, bytes);
  }

  void ServiceTx(uint32_t i) {
    Tx& s = tx[i];
    if (!s.gate.active.load() || sock == INVALID_SOCKET) return;
    const uint32_t ch = s.cfg.channels;
    if (s.cfg.codec == WHA_VORBIS) {
      const uint64_t now = Qpc();
      if (s.lastCodebookQpc == 0 || now - s.lastCodebookQpc > kCodebookRepeatMs * qpcFreq / 1000) {
        for (uint32_t k = 0; k < kWhaaCodebookRedundant; ++k)
          Send(s, s.codebookPacket.data(), s.codebookPacket.size(), nullptr, 0);
        s.lastCodebookQpc = now;
      }
      uint32_t n = 0;
      while ((n = s.ring.read(s.block.data(), kVorbisFeedFrames)) > 0) {
        s.vorbisPackets.clear();
        if (!s.enc.Encode(s.block.data(), n, s.vorbisPackets)) {
          s.c.errors.fetch_add(1);
          continue;
        }
        for (const auto& p : s.vorbisPackets) SendAudio(i, s, n, p.data(), p.size());
      }
      return;
    }
    while (s.ring.readable() >= s.packetFrames) {
      s.ring.read(s.block.data(), s.packetFrames);
      const std::vector<uint8_t> payload = BuildPcmPayload(s.block.data(), ch, s.packetFrames, s.cfg.codec);
      SendAudio(i, s, s.packetFrames, payload.data(), payload.size());
    }
  }

  // ---- Rx (network thread) ----

  void Push(Rx& s, const float* data, uint32_t frames) {
    s.drifted.clear();
    s.drift.process(data, frames, s.ratio, s.drifted);
    const uint32_t out = static_cast<uint32_t>(s.drifted.size() / s.cfg.channels);
    const uint32_t written = s.ring.write(s.drifted.data(), out);
    if (written < out) s.c.overflows.fetch_add(out - written);
    UpdateDrift(s);
  }

  // PI loop on smoothed ring fill -> resample ratio for the next packet.
  void UpdateDrift(Rx& s) {
    const uint64_t now = Qpc();
    const double dt = s.lastQpc ? static_cast<double>(now - s.lastQpc) / static_cast<double>(qpcFreq) : 0.0;
    s.lastQpc = now;
    const bool primed = s.primed.load();
    if (!primed) {  // (re)priming: fill is not meaningful; keep the drift estimate, re-take the baseline
      s.wasPrimed = false;
      s.baselineSet = false;
      return;
    }
    const double fill = static_cast<double>(s.ring.readable());
    if (!s.wasPrimed) {
      s.wasPrimed = true;
      s.primedQpc = now;
      s.fillEma = fill;
    }
    s.fillEma += (fill - s.fillEma) * (dt < kFillTauSec ? dt / kFillTauSec : 1.0);
    if (!s.baselineSet) {
      if (static_cast<double>(now - s.primedQpc) / static_cast<double>(qpcFreq) < kBaselineSec) return;
      s.baseline = s.fillEma;
      s.baselineSet = true;
    }
    const double err = s.fillEma - s.baseline;
    const double deadband = std::max(96.0, s.target.load() / 5.0);
    if (!s.driftEngaged.load() && std::fabs(err) < deadband) return;
    s.driftEngaged.store(true);
    const double rate = static_cast<double>(s.rate);
    const double maxCorr = kMaxDriftPpm * 1e-6;
    double corr = -(kDriftKp * err + kDriftKi * (s.integ + err * dt)) / rate;
    if (corr > -maxCorr && corr < maxCorr) s.integ += err * dt;  // anti-windup: freeze while clamped
    corr = std::clamp(corr, -maxCorr, maxCorr);
    s.ratio = 1.0 + corr;
    s.driftPpmMilli.store(static_cast<int32_t>(-corr * 1e9));
  }

  void OnCodebook(const uint8_t* buf, int n) {
    WHAACodebookHeader h;
    if (n < static_cast<int>(sizeof(h))) return void(owner->malformed_.fetch_add(1));
    std::memcpy(&h, buf, sizeof(h));
    if (!ValidateWhaaCodebook(h, nullptr) || static_cast<size_t>(n) != sizeof(h) + h.headersBytes)
      return void(owner->malformed_.fetch_add(1));
    Rx& s = rx[h.streamId];
    if (!s.gate.active.load()) return;
    if (s.cfg.codec != WHA_VORBIS || h.channels != s.cfg.channels) return void(s.c.dropped.fetch_add(1));
    const uint8_t* headers = buf + sizeof(h);
    if (s.decoderOpen && s.codebookHeaders.size() == h.headersBytes &&
        std::memcmp(s.codebookHeaders.data(), headers, h.headersBytes) == 0)
      return;  // redundant copy
    WHACodebook cb{h.streamId, h.sampleRate, h.channels, h.quality, std::vector<uint8_t>(headers, headers + h.headersBytes)};
    s.decoderOpen = s.dec.Open(cb);
    if (!s.decoderOpen) return void(s.c.errors.fetch_add(1));
    s.codebookHeaders = cb.headers;
    s.haveSeq = false;
    s.resample = h.sampleRate != s.rate;
    if (s.resample && !s.src.Open(h.sampleRate, s.rate, h.channels, kMaxTickFrames)) {
      s.decoderOpen = false;
      s.c.errors.fetch_add(1);
    }
  }

  void OnAudio(const uint8_t* buf, int n, const in_addr& from) {
    WHAAPacketHeader h;
    if (n < static_cast<int>(sizeof(h))) return void(owner->malformed_.fetch_add(1));
    std::memcpy(&h, buf, sizeof(h));
    if (!ValidateWhaaHeader(h, nullptr) || static_cast<size_t>(n) != sizeof(h) + h.payloadBytes)
      return void(owner->malformed_.fetch_add(1));
    Rx& s = rx[h.streamId];
    if (!s.gate.active.load()) return;
    if (s.peerFilter && from.s_addr != s.peer.s_addr) return void(s.c.dropped.fetch_add(1));
    if (h.codec != s.cfg.codec || h.channels != s.cfg.channels) return void(s.c.dropped.fetch_add(1));
    if (s.haveSeq) {
      const int32_t ahead = static_cast<int32_t>(h.sequence - s.nextSeq);
      // A jump beyond kSeqResync either way is a sender restart, not loss/lateness: resync to it.
      if (ahead < 0 && ahead > -kSeqResync) return void(s.c.dropped.fetch_add(1));  // late or duplicate
      if (ahead > 0 && ahead < kSeqResync) s.c.lost.fetch_add(static_cast<uint64_t>(ahead));
    }
    s.haveSeq = true;
    s.nextSeq = h.sequence + 1;
    s.c.packets.fetch_add(1);
    s.c.bytes.fetch_add(static_cast<uint64_t>(n));
    const uint8_t* payload = buf + sizeof(h);
    const uint32_t ch = h.channels;

    if (h.codec != WHA_VORBIS) {
      s.pcm.resize(static_cast<size_t>(h.frames) * ch);
      if (!ParsePcmPayload(payload, h.payloadBytes, s.pcm.data(), ch, h.frames, static_cast<WHACodec>(h.codec)))
        return void(owner->malformed_.fetch_add(1));
      Push(s, s.pcm.data(), h.frames);
      return;
    }
    if (!s.decoderOpen) return void(s.c.dropped.fetch_add(1));  // waiting for CODEBOOK
    s.decoded.clear();
    if (!s.dec.Decode(payload, h.payloadBytes, s.decoded)) return void(s.c.errors.fetch_add(1));
    const uint32_t frames = static_cast<uint32_t>(s.decoded.size() / ch);
    if (!s.resample) {
      Push(s, s.decoded.data(), frames);
      return;
    }
    for (uint32_t at = 0; at < frames; at += kMaxTickFrames) {
      const uint32_t chunk = frames - at < kMaxTickFrames ? frames - at : kMaxTickFrames;
      s.resampled.clear();
      if (!s.src.Process(s.decoded.data() + static_cast<size_t>(at) * ch, chunk, s.resampled))
        return void(s.c.errors.fetch_add(1));
      Push(s, s.resampled.data(), static_cast<uint32_t>(s.resampled.size() / ch));
    }
  }

  void ServiceRx() {
    if (sock == INVALID_SOCKET) return;
    for (;;) {
      sockaddr_in from{};
      int fromLen = sizeof(from);
      const int n = recvfrom(sock, reinterpret_cast<char*>(recvBuf.data()), static_cast<int>(recvBuf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
      if (n == SOCKET_ERROR) {
        const int e = WSAGetLastError();
        if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;  // ICMP port unreachable / oversized: skip
        return;                                               // WSAEWOULDBLOCK: drained
      }
      if (n < 12) {
        owner->malformed_.fetch_add(1);
        continue;
      }
      uint32_t magic, version, type;
      std::memcpy(&magic, recvBuf.data(), 4);
      std::memcpy(&version, recvBuf.data() + 4, 4);
      std::memcpy(&type, recvBuf.data() + 8, 4);
      if (!IsValidWhaaMagic(magic) || !IsValidWhaaVersion(version)) {
        owner->malformed_.fetch_add(1);
        continue;
      }
      if (type == WHAA_CODEBOOK) OnCodebook(recvBuf.data(), n);
      else if (type == WHAA_AUDIO) OnAudio(recvBuf.data(), n, from.sin_addr);
      // WHAA_HELLO (6981 discovery) is not handled on the audio port.
    }
  }
};

WHANetworkEngine::WHANetworkEngine(const WHASlotTable* table, WHANetworkOptions options) : impl_(new Impl) {
  impl_->owner = this;
  impl_->table = table;
  impl_->options = options;
}

WHANetworkEngine::~WHANetworkEngine() {
  stop();
  delete impl_;
}

bool WHANetworkEngine::start() {
  if (impl_->thread) return true;
  if (!impl_->table) return false;
  impl_->quit = false;
  impl_->reconfigure = true;
  impl_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  impl_->thread = CreateThread(nullptr, 0, Impl::ThreadProc, impl_, 0, nullptr);
  return impl_->thread != nullptr;
}

void WHANetworkEngine::stop() {
  if (!impl_->thread) return;
  impl_->quit = true;
  SetEvent(impl_->wake);
  WaitForSingleObject(impl_->thread, INFINITE);
  CloseHandle(impl_->thread);
  CloseHandle(impl_->wake);
  impl_->thread = nullptr;
  impl_->wake = nullptr;
}

void WHANetworkEngine::requestReconfigure() {
  impl_->reconfigure = true;
  if (impl_->wake) SetEvent(impl_->wake);
}

void WHANetworkEngine::processTick(const float* masterOut, float* masterIn, size_t slotStride, uint32_t frames) {
  if (!impl_->thread || !masterOut || !masterIn) return;
  if (frames > kMaxTickFrames) frames = kMaxTickFrames;
  const WHASlotTable& t = *impl_->table;
  bool anyTx = false;

  for (uint32_t i = 0; i < kNetStreams; ++i) {
    Impl::Tx& s = impl_->tx[i];
    if (!s.gate.enter()) continue;
    const uint32_t ch = s.ring.channels();
    float* g = s.gather.data();
    std::memset(g, 0, sizeof(float) * frames * ch);
    for (uint32_t oi = 0; oi < t.masterOutCount; ++oi) {
      const WHASlot& slot = t.masterOut[oi];
      if (slot.type != SLOT_NETWORK || slot.streamId != static_cast<int32_t>(i) || !slot.enabled) continue;
      if (slot.srcChannel < 0 || static_cast<uint32_t>(slot.srcChannel) >= ch) continue;
      const float* src = masterOut + oi * slotStride;
      for (uint32_t f = 0; f < frames; ++f) g[f * ch + slot.srcChannel] += src[f];
    }
    const uint32_t written = s.ring.write(g, frames);
    if (written < frames) s.c.overflows.fetch_add(frames - written);
    s.gate.leave();
    anyTx = true;
  }
  if (anyTx) SetEvent(impl_->wake);

  for (uint32_t i = 0; i < kNetStreams; ++i) {
    Impl::Rx& s = impl_->rx[i];
    bool have = false;
    if (s.gate.enter()) {
      const uint32_t ch = s.ring.channels();
      const uint32_t target = s.target.load();
      const uint32_t avail = s.ring.readable();
      if (!s.primed.load() && avail >= target) s.primed.store(true);
      if (s.primed.load()) {
        if (avail < frames) {
          s.primed.store(false);  // underrun: silence until the jitter target refills
          s.c.underruns.fetch_add(1);
        } else {
          if (avail > 2 * target + 2 * frames) s.c.dropped.fetch_add(s.ring.discard(avail - target));  // drift trim
          s.ring.read(s.scratch.data(), frames);
          have = true;
        }
      }
      // Scatter while still inside the gate: the network thread may free scratch right after leave().
      for (uint32_t ii = 0; have && ii < t.masterInCount; ++ii) {
        const WHASlot& slot = t.masterIn[ii];
        if (slot.type != SLOT_NETWORK || slot.streamId != static_cast<int32_t>(i)) continue;
        float* dst = masterIn + ii * slotStride;
        if (slot.enabled && slot.srcChannel >= 0 && static_cast<uint32_t>(slot.srcChannel) < ch) {
          const float* d = s.scratch.data();
          for (uint32_t f = 0; f < frames; ++f) dst[f] = d[f * ch + slot.srcChannel];
        } else {
          std::memset(dst, 0, sizeof(float) * frames);
        }
      }
      s.gate.leave();
    }
    for (uint32_t ii = 0; !have && ii < t.masterInCount; ++ii) {  // unprimed / inactive: silence
      const WHASlot& slot = t.masterIn[ii];
      if (slot.type == SLOT_NETWORK && slot.streamId == static_cast<int32_t>(i))
        std::memset(masterIn + ii * slotStride, 0, sizeof(float) * frames);
    }
  }
}

const WHANetworkCounters& WHANetworkEngine::txCounters(uint32_t stream) const { return impl_->tx[stream % kNetStreams].c; }
const WHANetworkCounters& WHANetworkEngine::rxCounters(uint32_t stream) const { return impl_->rx[stream % kNetStreams].c; }
bool WHANetworkEngine::txActive(uint32_t stream) const { return impl_->tx[stream % kNetStreams].gate.active.load(); }
bool WHANetworkEngine::rxActive(uint32_t stream) const { return impl_->rx[stream % kNetStreams].gate.active.load(); }
bool WHANetworkEngine::rxPrimed(uint32_t stream) const { return impl_->rx[stream % kNetStreams].primed.load(); }
bool WHANetworkEngine::rxDriftEngaged(uint32_t stream) const { return impl_->rx[stream % kNetStreams].driftEngaged.load(); }
double WHANetworkEngine::rxDriftPpm(uint32_t stream) const {
  return impl_->rx[stream % kNetStreams].driftPpmMilli.load() / 1000.0;
}
uint32_t WHANetworkEngine::rxFillFrames(uint32_t stream) const {
  const auto& s = impl_->rx[stream % kNetStreams];
  return s.gate.active.load() ? s.ring.readable() : 0;
}
uint32_t WHANetworkEngine::rxTargetFrames(uint32_t stream) const { return impl_->rx[stream % kNetStreams].target.load(); }
std::string WHANetworkEngine::socketError() const {
  std::lock_guard<std::mutex> lock(impl_->errorMutex);
  return impl_->socketError;
}

}  // namespace wha
