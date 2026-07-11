// Aether Engine — XAudio2 implementation of the audio service.
#include "audio.h"
#include "../core/log.h"
#include <windows.h>
#include <xaudio2.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

namespace ae {

namespace {

// A decoded PCM clip: the raw sample bytes plus the format they're in. The
// bytes must outlive every voice that references them — they live for the whole
// AudioEngine lifetime in the sound cache, so that always holds.
struct Sound {
    std::vector<uint8_t> data;
    WAVEFORMATEX fmt{};
    bool valid = false;
};

// Parses a minimal RIFF/WAVE container: the "fmt " chunk (PCM/16-bit typically,
// but any WAVEFORMATEX XAudio2 accepts) and the "data" chunk. Hand-rolled to
// match the engine's from-scratch loaders (glTF/JSON/image).
bool loadWav(const std::string& path, Sound& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    if (buf.size() < 44) return false;
    auto tag = [&](size_t o, const char* s) { return std::memcmp(&buf[o], s, 4) == 0; };
    auto u32 = [&](size_t o) {
        return (uint32_t)buf[o] | ((uint32_t)buf[o + 1] << 8) |
               ((uint32_t)buf[o + 2] << 16) | ((uint32_t)buf[o + 3] << 24);
    };
    if (!tag(0, "RIFF") || !tag(8, "WAVE")) return false;

    bool haveFmt = false, haveData = false;
    size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        uint32_t sz = u32(pos + 4);
        size_t body = pos + 8;
        if (body + sz > buf.size()) break;
        if (tag(pos, "fmt ") && sz >= 16) {
            WAVEFORMATEX& w = out.fmt;
            w.wFormatTag = (WORD)(buf[body] | (buf[body + 1] << 8));
            w.nChannels = (WORD)(buf[body + 2] | (buf[body + 3] << 8));
            w.nSamplesPerSec = u32(body + 4);
            w.nAvgBytesPerSec = u32(body + 8);
            w.nBlockAlign = (WORD)(buf[body + 12] | (buf[body + 13] << 8));
            w.wBitsPerSample = (WORD)(buf[body + 14] | (buf[body + 15] << 8));
            w.cbSize = 0;
            haveFmt = true;
        } else if (tag(pos, "data")) {
            out.data.assign(buf.begin() + body, buf.begin() + body + sz);
            haveData = true;
        }
        pos = body + sz + (sz & 1); // chunks are word-aligned
    }
    out.valid = haveFmt && haveData && out.fmt.nChannels > 0;
    return out.valid;
}

} // namespace

// ---- implementation state --------------------------------------------------

struct Voice {
    IXAudio2SourceVoice* src = nullptr;
    SoundId sound = -1;
    float volume = 1.0f;
    bool positional = false;
    bool loop = false;
    bool oneShot = false; // reaped automatically by update()
    Vec3 pos{0, 0, 0};
    float minDist = 1.0f;
    float maxDist = 40.0f;
};

struct AudioImpl {
    IXAudio2* xaudio = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    UINT32 dstChannels = 2;

    std::vector<Sound> sounds;
    std::unordered_map<std::string, SoundId> soundByPath;

    std::map<VoiceId, Voice> voices; // ordered: fine, counts stay small
    VoiceId nextVoice = 1;

    Vec3 listenerPos{0, 0, 0};
    Vec3 listenerFwd{0, 0, -1};
    Vec3 listenerUp{0, 1, 0};
};

AudioEngine& audioEngine() {
    static AudioEngine g;
    return g;
}

bool AudioEngine::init() {
    if (impl_) return true;
    // XAudio2 needs COM. The process may already be initialized (WIC, GUIDs) in
    // a different apartment — either mode works, so ignore RPC_E_CHANGED_MODE.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    auto* impl = new AudioImpl();
    HRESULT hr = XAudio2Create(&impl->xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        AE_WARN("[Audio] XAudio2Create failed (0x%08lx) — audio disabled", hr);
        delete impl;
        return false;
    }
    hr = impl->xaudio->CreateMasteringVoice(&impl->master);
    if (FAILED(hr)) {
        AE_WARN("[Audio] CreateMasteringVoice failed (0x%08lx) — audio disabled", hr);
        impl->xaudio->Release();
        delete impl;
        return false;
    }
    XAUDIO2_VOICE_DETAILS det{};
    impl->master->GetVoiceDetails(&det);
    impl->dstChannels = det.InputChannels ? det.InputChannels : 2;
    impl_ = impl;
    AE_LOG("[Audio] XAudio2 ready (%u output channels)", impl->dstChannels);
    return true;
}

void AudioEngine::shutdown() {
    if (!impl_) return;
    for (auto& kv : impl_->voices)
        if (kv.second.src) { kv.second.src->Stop(); kv.second.src->DestroyVoice(); }
    impl_->voices.clear();
    if (impl_->master) impl_->master->DestroyVoice();
    if (impl_->xaudio) impl_->xaudio->Release();
    delete impl_;
    impl_ = nullptr;
}

SoundId AudioEngine::loadSound(const std::string& absPath) {
    if (!impl_) return -1;
    auto it = impl_->soundByPath.find(absPath);
    if (it != impl_->soundByPath.end()) return it->second;

    Sound s;
    if (!loadWav(absPath, s)) {
        AE_WARN("[Audio] failed to load wav: %s", absPath.c_str());
        impl_->soundByPath[absPath] = -1; // negative-cache so we warn once
        return -1;
    }
    SoundId id = (SoundId)impl_->sounds.size();
    AE_LOG("[Audio] loaded %s (%u Hz, %u ch)", absPath.c_str(), s.fmt.nSamplesPerSec, s.fmt.nChannels);
    impl_->sounds.push_back(std::move(s));
    impl_->soundByPath[absPath] = id;
    return id;
}

// Recomputes SetOutputMatrix (pan+attenuation) for one positional voice.
static void apply3D(AudioImpl* impl, Voice& v) {
    if (!v.src || !v.positional) return;
    const Sound& snd = impl->sounds[v.sound];
    UINT32 srcCh = snd.fmt.nChannels;
    UINT32 dstCh = impl->dstChannels;

    Vec3 rel = v.pos - impl->listenerPos;
    float dist = length(rel);

    // Inverse-distance falloff: full volume within minDist, ~minDist/dist beyond,
    // with a short linear fade to silence as it approaches maxDist.
    float refDist = v.minDist > 0.01f ? v.minDist : 0.01f;
    float maxDist = v.maxDist > refDist ? v.maxDist : refDist + 1.0f;
    float att = dist <= refDist ? 1.0f : refDist / dist;
    if (dist >= maxDist) att = 0.0f;
    else if (dist > maxDist * 0.8f) att *= (maxDist - dist) / (maxDist * 0.2f);

    // Constant-power pan from the source's angle around the listener basis.
    float pan = 0.0f; // -1 left .. +1 right
    if (dist > 1e-3f) {
        Vec3 fwd = normalize(impl->listenerFwd);
        Vec3 right = normalize(cross(fwd, normalize(impl->listenerUp)));
        pan = clampf(dot(normalize(rel), right), -1.0f, 1.0f);
    }
    float lGain = std::sqrt(0.5f * (1.0f - pan));
    float rGain = std::sqrt(0.5f * (1.0f + pan));

    float matrix[8 * 8] = {}; // srcCh * dstCh, capped well above real layouts
    for (UINT32 c = 0; c < srcCh; ++c) {
        if (dstCh >= 2) {
            matrix[0 * srcCh + c] = lGain * att; // dst ch0 (L)
            matrix[1 * srcCh + c] = rGain * att; // dst ch1 (R)
        } else {
            matrix[c] = att; // mono output
        }
    }
    v.src->SetOutputMatrix(nullptr, srcCh, dstCh, matrix);
}

VoiceId AudioEngine::play(SoundId sound, float volume, bool loop, bool positional,
                          float minDistance, float maxDistance) {
    if (!impl_ || sound < 0 || sound >= (SoundId)impl_->sounds.size()) return 0;
    const Sound& snd = impl_->sounds[sound];
    if (!snd.valid) return 0;

    IXAudio2SourceVoice* src = nullptr;
    if (FAILED(impl_->xaudio->CreateSourceVoice(&src, &snd.fmt))) return 0;

    XAUDIO2_BUFFER buf{};
    buf.AudioBytes = (UINT32)snd.data.size();
    buf.pAudioData = snd.data.data();
    buf.Flags = XAUDIO2_END_OF_STREAM;
    buf.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
    src->SubmitSourceBuffer(&buf);
    src->SetVolume(volume);

    VoiceId id = impl_->nextVoice++;
    Voice v;
    v.src = src;
    v.sound = sound;
    v.volume = volume;
    v.positional = positional;
    v.loop = loop;
    v.minDist = minDistance;
    v.maxDist = maxDistance;
    auto& stored = (impl_->voices[id] = v);
    if (positional) apply3D(impl_, stored);
    src->Start(0);
    return id;
}

void AudioEngine::playOneShot(SoundId sound, float volume) {
    VoiceId id = play(sound, volume, /*loop=*/false, /*positional=*/false);
    if (id) impl_->voices[id].oneShot = true;
}

void AudioEngine::setVoicePosition(VoiceId v, const Vec3& worldPos) {
    if (!impl_) return;
    auto it = impl_->voices.find(v);
    if (it == impl_->voices.end()) return;
    it->second.pos = worldPos;
    apply3D(impl_, it->second);
}

void AudioEngine::setVoiceVolume(VoiceId v, float volume) {
    if (!impl_) return;
    auto it = impl_->voices.find(v);
    if (it == impl_->voices.end() || !it->second.src) return;
    it->second.volume = volume;
    it->second.src->SetVolume(volume);
}

bool AudioEngine::voiceActive(VoiceId v) const {
    if (!impl_) return false;
    auto it = impl_->voices.find(v);
    if (it == impl_->voices.end() || !it->second.src) return false;
    if (it->second.loop) return true;
    XAUDIO2_VOICE_STATE st{};
    it->second.src->GetState(&st);
    return st.BuffersQueued > 0;
}

void AudioEngine::stopVoice(VoiceId v) {
    if (!impl_) return;
    auto it = impl_->voices.find(v);
    if (it == impl_->voices.end()) return;
    if (it->second.src) { it->second.src->Stop(); it->second.src->DestroyVoice(); }
    impl_->voices.erase(it);
}

void AudioEngine::setListener(const Vec3& pos, const Vec3& forward, const Vec3& up) {
    if (!impl_) return;
    impl_->listenerPos = pos;
    impl_->listenerFwd = forward;
    impl_->listenerUp = up;
}

void AudioEngine::update(float) {
    if (!impl_) return;
    // Re-pan every positional voice against the (possibly moved) listener, and
    // reap finished one-shots so their voices don't accumulate.
    for (auto it = impl_->voices.begin(); it != impl_->voices.end();) {
        Voice& v = it->second;
        if (v.positional) apply3D(impl_, v);
        if (v.oneShot && v.src) {
            XAUDIO2_VOICE_STATE st{};
            v.src->GetState(&st);
            if (st.BuffersQueued == 0) {
                v.src->DestroyVoice();
                it = impl_->voices.erase(it);
                continue;
            }
        }
        ++it;
    }
}

} // namespace ae
