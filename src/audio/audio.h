// Aether Engine — audio service.
//
// A thin, dependency-free wrapper over XAudio2 (Windows SDK — same "use the OS"
// approach as WIC for images and GDI for fonts; no vendored library, no shipped
// redistributable). One global AudioEngine mixes fire-and-forget one-shots and
// persistent, positional voices. 3D is done by hand (distance attenuation +
// constant-power stereo pan against the listener basis) so nothing beyond
// xaudio2.lib is required.
//
// Lifecycle: the app layer (runtime / editor Play) calls init() once,
// setListener()+update() each frame, and shutdown() at exit. Gameplay code and
// the AudioSource component talk to it through audioEngine().
#pragma once
#include "../core/math3d.h"
#include <cstdint>
#include <string>

namespace ae {

// Opaque handle to a decoded, cached clip (>=0 valid, -1 invalid).
using SoundId = int;
// Opaque handle to a live voice (0 invalid). Stable across the voice's life.
using VoiceId = uint32_t;

struct AudioImpl; // XAudio2 state, hidden from this header.

class AudioEngine {
public:
    bool init();
    void shutdown();
    bool available() const { return impl_ != nullptr; }

    // Loads (and caches) a PCM .wav by absolute path. Repeated calls for the
    // same path return the same SoundId. -1 on failure or if audio is disabled.
    SoundId loadSound(const std::string& absPath);

    // Fire-and-forget non-positional playback; reaped automatically when done.
    void playOneShot(SoundId sound, float volume = 1.0f);

    // Persistent voice (looping or one-shot) that can be positioned in 3D and is
    // owned by the caller until stopVoice(). Returns 0 on failure. For positional
    // voices, minDistance is the radius of full volume and maxDistance the radius
    // beyond which it's silent (inverse-distance rolloff in between).
    VoiceId play(SoundId sound, float volume, bool loop, bool positional,
                 float minDistance = 1.0f, float maxDistance = 40.0f);
    void setVoicePosition(VoiceId v, const Vec3& worldPos);
    void setVoiceVolume(VoiceId v, float volume);
    bool voiceActive(VoiceId v) const;   // false once a non-looping voice ends
    void stopVoice(VoiceId v);

    // The "ears": position + orientation the 3D mix is computed against. Fed
    // from the resolved gameplay camera each frame.
    void setListener(const Vec3& pos, const Vec3& forward, const Vec3& up);

    // Commits 3D parameters and reaps finished one-shots. Call once per frame.
    void update(float dt);

private:
    AudioImpl* impl_ = nullptr;
};

// The process-wide audio service (lives in AetherCore.dll).
AudioEngine& audioEngine();

} // namespace ae
