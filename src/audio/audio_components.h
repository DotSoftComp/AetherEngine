// Aether Engine — AudioSource: an entity that emits sound.
//
// Drop it on any entity, point it at a .wav, and it plays during Play/runtime
// (never in edit mode — onStart only fires when behaviors tick). Positional
// sources attenuate and pan against the listener (the active camera); 2D
// sources play flat. The live voice is owned here and released in the
// destructor, so PIE Stop, entity destroy, or scene reload can never leak a
// playing sound.
#pragma once
#include "../engine/component.h"
#include "../engine/entity.h"
#include "../engine/assets.h"
#include "../engine/reflect.h"
#include "audio.h"

namespace ae {

class AudioSourceComponent : public Component {
public:
    std::string clip;          // project-relative .wav path (drag from Content Browser)
    float volume = 1.0f;
    bool loop = false;
    bool spatial = true;       // 3D attenuation + pan vs. the listener
    bool playOnStart = true;
    float minDistance = 5.0f;  // full volume within this radius
    float maxDistance = 50.0f; // silent beyond this radius

    ~AudioSourceComponent() override { stop(); }

    const char* typeName() const override { return "AudioSource"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("clip", clip, {PropKind::AudioClip, "Clip"});
        v.visit("volume", volume, {PropKind::SliderNorm, "Volume", 0.01f, 0.0f, 1.0f});
        v.visit("loop", loop, {PropKind::Default, "Loop"});
        v.visit("spatial", spatial, {PropKind::Default, "3D / spatial"});
        v.visit("playOnStart", playOnStart, {PropKind::Default, "Play on start"});
        v.visit("minDistance", minDistance, {PropKind::Default, "Min distance", 0.1f, 0.0f, 500.0f});
        v.visit("maxDistance", maxDistance, {PropKind::Default, "Max distance", 0.5f, 0.0f, 2000.0f});
    }
    void onDeserialized(AssetLibrary& assets) override {
        // Resolve now; the actual load may return -1 if the audio device isn't up
        // yet (the editor loads scenes before initializing audio) — play() retries.
        resolved_ = clip.empty() ? std::string() : assets.resolvePath(clip);
        sound_ = resolved_.empty() ? -1 : audioEngine().loadSound(resolved_);
    }

    void onStart() override {
        if (playOnStart) play();
    }
    void onUpdate(float) override {
        if (spatial && voice_ && audioEngine().voiceActive(voice_))
            audioEngine().setVoicePosition(voice_, entity().worldPosition());
    }

    // Gameplay/dialogue can trigger these directly.
    void play() {
        stop();
        // Retry the load: onDeserialized may have run before audio was ready.
        if (sound_ < 0 && !resolved_.empty()) sound_ = audioEngine().loadSound(resolved_);
        if (sound_ < 0) return;
        voice_ = audioEngine().play(sound_, volume, loop, spatial, minDistance, maxDistance);
        if (voice_ && spatial)
            audioEngine().setVoicePosition(voice_, entity().worldPosition());
    }
    void stop() {
        if (voice_) { audioEngine().stopVoice(voice_); voice_ = 0; }
    }

private:
    std::string resolved_; // absolute path resolved at deserialize
    SoundId sound_ = -1;
    VoiceId voice_ = 0;
};

} // namespace ae
