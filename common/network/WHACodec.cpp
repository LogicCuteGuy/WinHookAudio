// WHACodec — WHAA Vorbis encode/decode (libvorbis 1.3.7 + libogg 1.3.5) and SRC (r8brain-free 7.5).
// Vocabulary: Network Stream, Worker.
// Built with WHA_HAVE_VORBIS / WHA_HAVE_R8BRAIN only when the third_party payload is vendored (ADR 0009);
// without it every Open() returns false and callers keep PCM.

#include "WHANetwork.h"

#include <cstring>

#if WHA_HAVE_VORBIS
#pragma warning(push, 0)
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#pragma warning(pop)
#endif

#if WHA_HAVE_R8BRAIN
#pragma warning(push, 0)
#include <CDSPResampler.h>
#pragma warning(pop)
#endif

namespace wha {

namespace {

void PutU32(std::vector<uint8_t>& dst, uint32_t v) {
  const size_t at = dst.size();
  dst.resize(at + 4);
  std::memcpy(dst.data() + at, &v, 4);
}

// Splits CODEBOOK headers into the 3 Vorbis header packets; false if the layout is malformed.
bool SplitCodebookHeaders(const std::vector<uint8_t>& headers, const uint8_t* part[3], uint32_t len[3]) {
  if (headers.size() < 12) return false;
  uint64_t total = 12;
  for (int i = 0; i < 3; ++i) {
    std::memcpy(&len[i], headers.data() + 4 * i, 4);
    if (len[i] == 0) return false;
    total += len[i];
  }
  if (total != headers.size()) return false;
  const uint8_t* p = headers.data() + 12;
  for (int i = 0; i < 3; ++i) {
    part[i] = p;
    p += len[i];
  }
  return true;
}

}  // namespace

bool ValidateCodebook(const WHACodebook& cb) {
  if (cb.streamId >= kNetStreams) return false;
  if (cb.sampleRate != 44100 && cb.sampleRate != 48000 && cb.sampleRate != 96000) return false;
  if (!IsValidNetworkChannels(WHA_VORBIS, cb.channels)) return false;
  if (cb.quality < 0.1f || cb.quality > 1.0f) return false;
  if (cb.headers.size() > kWhaaMaxPayload) return false;
  const uint8_t* part[3];
  uint32_t len[3];
  return SplitCodebookHeaders(cb.headers, part, len);
}

// ---- Vorbis ----

#if WHA_HAVE_VORBIS

bool VorbisAvailable() { return true; }

struct WHAVorbisEncoder::Impl {
  vorbis_info vi{};
  vorbis_comment vc{};
  vorbis_dsp_state vd{};
  vorbis_block vb{};
  uint32_t channels = 0;

  Impl() = default;
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl() {
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
  }

  void Drain(std::vector<std::vector<uint8_t>>& packets) {
    while (vorbis_analysis_blockout(&vd, &vb) == 1) {
      vorbis_analysis(&vb, nullptr);
      vorbis_bitrate_addblock(&vb);
      ogg_packet op{};
      while (vorbis_bitrate_flushpacket(&vd, &op) == 1)
        packets.emplace_back(op.packet, op.packet + op.bytes);
    }
  }
};

WHAVorbisEncoder::WHAVorbisEncoder() = default;
WHAVorbisEncoder::~WHAVorbisEncoder() = default;

bool WHAVorbisEncoder::Open(uint32_t streamId, uint32_t sampleRate, uint32_t channels, float quality, WHACodebook& codebook) {
  Close();
  codebook = WHACodebook{streamId, sampleRate, channels, quality, {}};
  if (!ValidateWhaaCodebook(WHAACodebookHeader{kWhaaMagic, kWhaaVersion, WHAA_CODEBOOK, streamId, sampleRate, channels, quality, 1}, nullptr))
    return false;

  auto impl = std::make_unique<Impl>();
  vorbis_info_init(&impl->vi);
  // Q0.1–1.0 maps directly onto libvorbis VBR base quality.
  if (vorbis_encode_init_vbr(&impl->vi, static_cast<long>(channels), static_cast<long>(sampleRate), quality) != 0) {
    vorbis_info_clear(&impl->vi);
    return false;
  }
  vorbis_comment_init(&impl->vc);
  vorbis_comment_add_tag(&impl->vc, "ENCODER", "WinHookAudio WHAA");
  if (vorbis_analysis_init(&impl->vd, &impl->vi) != 0) return false;
  vorbis_block_init(&impl->vd, &impl->vb);
  impl->channels = channels;

  ogg_packet hdr[3]{};
  if (vorbis_analysis_headerout(&impl->vd, &impl->vc, &hdr[0], &hdr[1], &hdr[2]) != 0) return false;
  for (const ogg_packet& h : hdr) PutU32(codebook.headers, static_cast<uint32_t>(h.bytes));
  for (const ogg_packet& h : hdr) codebook.headers.insert(codebook.headers.end(), h.packet, h.packet + h.bytes);
  if (!ValidateCodebook(codebook)) return false;

  impl_ = std::move(impl);
  return true;
}

bool WHAVorbisEncoder::Encode(const float* interleaved, uint32_t frames, std::vector<std::vector<uint8_t>>& packets) {
  if (!impl_ || !interleaved || frames == 0) return false;
  float** buf = vorbis_analysis_buffer(&impl_->vd, static_cast<int>(frames));
  const uint32_t ch = impl_->channels;
  for (uint32_t f = 0; f < frames; ++f)
    for (uint32_t c = 0; c < ch; ++c) buf[c][f] = interleaved[f * ch + c];
  if (vorbis_analysis_wrote(&impl_->vd, static_cast<int>(frames)) != 0) return false;
  impl_->Drain(packets);
  return true;
}

bool WHAVorbisEncoder::Flush(std::vector<std::vector<uint8_t>>& packets) {
  if (!impl_) return false;
  if (vorbis_analysis_wrote(&impl_->vd, 0) != 0) return false;
  impl_->Drain(packets);
  return true;
}

void WHAVorbisEncoder::Close() { impl_.reset(); }

struct WHAVorbisDecoder::Impl {
  vorbis_info vi{};
  vorbis_comment vc{};
  vorbis_dsp_state vd{};
  vorbis_block vb{};
  bool dspReady = false;
  ogg_int64_t packetno = 3;

  Impl() {
    vorbis_info_init(&vi);
    vorbis_comment_init(&vc);
  }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl() {
    if (dspReady) {
      vorbis_block_clear(&vb);
      vorbis_dsp_clear(&vd);
    }
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
  }
};

WHAVorbisDecoder::WHAVorbisDecoder() = default;
WHAVorbisDecoder::~WHAVorbisDecoder() = default;

bool WHAVorbisDecoder::Open(const WHACodebook& codebook) {
  Close();
  if (!ValidateCodebook(codebook)) return false;
  const uint8_t* part[3];
  uint32_t len[3];
  if (!SplitCodebookHeaders(codebook.headers, part, len)) return false;

  auto impl = std::make_unique<Impl>();
  for (int i = 0; i < 3; ++i) {
    ogg_packet op{};
    op.packet = const_cast<unsigned char*>(part[i]);
    op.bytes = len[i];
    op.b_o_s = i == 0 ? 1 : 0;
    op.packetno = i;
    if (vorbis_synthesis_headerin(&impl->vi, &impl->vc, &op) != 0) return false;
  }
  // The CODEBOOK fields must agree with what the headers actually describe.
  if (static_cast<uint32_t>(impl->vi.channels) != codebook.channels ||
      static_cast<uint32_t>(impl->vi.rate) != codebook.sampleRate)
    return false;
  if (vorbis_synthesis_init(&impl->vd, &impl->vi) != 0) return false;
  vorbis_block_init(&impl->vd, &impl->vb);
  impl->dspReady = true;
  impl_ = std::move(impl);
  return true;
}

bool WHAVorbisDecoder::Decode(const uint8_t* packet, uint32_t bytes, std::vector<float>& interleavedOut) {
  if (!impl_ || !packet || bytes == 0 || bytes > kWhaaMaxPayload) return false;
  ogg_packet op{};
  op.packet = const_cast<unsigned char*>(packet);
  op.bytes = bytes;
  op.packetno = impl_->packetno++;
  if (vorbis_synthesis(&impl_->vb, &op) != 0) return false;
  if (vorbis_synthesis_blockin(&impl_->vd, &impl_->vb) != 0) return false;
  const size_t ch = static_cast<size_t>(impl_->vi.channels);
  float** pcm = nullptr;
  int n = 0;
  while ((n = vorbis_synthesis_pcmout(&impl_->vd, &pcm)) > 0) {
    const size_t at = interleavedOut.size();
    interleavedOut.resize(at + static_cast<size_t>(n) * ch);
    float* dst = interleavedOut.data() + at;
    for (int f = 0; f < n; ++f)
      for (size_t c = 0; c < ch; ++c) dst[static_cast<size_t>(f) * ch + c] = pcm[c][f];
    vorbis_synthesis_read(&impl_->vd, n);
  }
  return true;
}

void WHAVorbisDecoder::Close() { impl_.reset(); }

#else  // !WHA_HAVE_VORBIS

bool VorbisAvailable() { return false; }

struct WHAVorbisEncoder::Impl {};
WHAVorbisEncoder::WHAVorbisEncoder() = default;
WHAVorbisEncoder::~WHAVorbisEncoder() = default;
bool WHAVorbisEncoder::Open(uint32_t, uint32_t, uint32_t, float, WHACodebook&) { return false; }
bool WHAVorbisEncoder::Encode(const float*, uint32_t, std::vector<std::vector<uint8_t>>&) { return false; }
bool WHAVorbisEncoder::Flush(std::vector<std::vector<uint8_t>>&) { return false; }
void WHAVorbisEncoder::Close() {}

struct WHAVorbisDecoder::Impl {};
WHAVorbisDecoder::WHAVorbisDecoder() = default;
WHAVorbisDecoder::~WHAVorbisDecoder() = default;
bool WHAVorbisDecoder::Open(const WHACodebook&) { return false; }
bool WHAVorbisDecoder::Decode(const uint8_t*, uint32_t, std::vector<float>&) { return false; }
void WHAVorbisDecoder::Close() {}

#endif

// ---- SRC ----

#if WHA_HAVE_R8BRAIN

bool ResamplerAvailable() { return true; }

struct WHAResampler::Impl {
  std::vector<std::unique_ptr<r8b::CDSPResampler24>> perChannel;
  std::vector<double> in;
  uint32_t channels = 0;
  uint32_t maxInFrames = 0;
};

WHAResampler::WHAResampler() = default;
WHAResampler::~WHAResampler() = default;

bool WHAResampler::Open(uint32_t inRate, uint32_t outRate, uint32_t channels, uint32_t maxInFrames) {
  Close();
  if (inRate == 0 || outRate == 0 || maxInFrames == 0 || maxInFrames > 65536) return false;
  if (!IsValidNetworkChannels(WHA_PCM_F32, channels)) return false;
  auto impl = std::make_unique<Impl>();
  impl->channels = channels;
  impl->maxInFrames = maxInFrames;
  impl->in.resize(maxInFrames);
  for (uint32_t c = 0; c < channels; ++c)
    impl->perChannel.push_back(std::make_unique<r8b::CDSPResampler24>(inRate, outRate, static_cast<int>(maxInFrames)));
  impl_ = std::move(impl);
  return true;
}

bool WHAResampler::Process(const float* interleaved, uint32_t frames, std::vector<float>& interleavedOut) {
  if (!impl_ || !interleaved || frames == 0 || frames > impl_->maxInFrames) return false;
  const uint32_t ch = impl_->channels;
  const size_t base = interleavedOut.size();
  int produced = -1;
  for (uint32_t c = 0; c < ch; ++c) {
    for (uint32_t f = 0; f < frames; ++f) impl_->in[f] = interleaved[f * ch + c];
    double* op = nullptr;
    const int n = impl_->perChannel[c]->process(impl_->in.data(), static_cast<int>(frames), op);
    // Every channel runs the same filter, so they must advance in lockstep.
    if (produced < 0) {
      produced = n;
      interleavedOut.resize(base + static_cast<size_t>(n) * ch);
    } else if (n != produced) {
      interleavedOut.resize(base);
      return false;
    }
    float* dst = interleavedOut.data() + base;
    for (int f = 0; f < n; ++f) dst[static_cast<size_t>(f) * ch + c] = static_cast<float>(op[f]);
  }
  return true;
}

void WHAResampler::Close() { impl_.reset(); }

#else  // !WHA_HAVE_R8BRAIN

bool ResamplerAvailable() { return false; }

struct WHAResampler::Impl {};
WHAResampler::WHAResampler() = default;
WHAResampler::~WHAResampler() = default;
bool WHAResampler::Open(uint32_t, uint32_t, uint32_t, uint32_t) { return false; }
bool WHAResampler::Process(const float*, uint32_t, std::vector<float>&) { return false; }
void WHAResampler::Close() {}

#endif

}  // namespace wha
