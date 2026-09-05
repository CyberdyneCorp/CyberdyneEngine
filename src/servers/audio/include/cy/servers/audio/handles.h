#pragma once
// The audio server's object families, as generational handles. Task 4.3.5.
//
// `engine-architecture` — a server "addresses every object through opaque generational handles".
// The generation counter earns its place here more than almost anywhere else in the engine: a voice
// finishes on the audio thread, its slot is reused by the next one-shot, and a gameplay handle held
// across that must answer "no" rather than turn the volume down on somebody else's sound.
//
// FOUR FAMILIES:
//
//   AudioBus       a node of the mixing graph
//   AudioClip      decoded or memory-resident sample data, registered once and played many times
//   AudioVoice     one playing instance of a clip
//   AudioListener  where the ears are — several, for split screen

#include <cy/core/values/handle.h>

namespace cy::audio {

CY_HANDLE_TAG(AudioBus);
CY_HANDLE_TAG(AudioClip);
CY_HANDLE_TAG(AudioVoice);
CY_HANDLE_TAG(AudioListener);

using BusHandle = Handle<AudioBusTag>;
using ClipHandle = Handle<AudioClipTag>;
using VoiceHandle = Handle<AudioVoiceTag>;
using ListenerHandle = Handle<AudioListenerTag>;

}  // namespace cy::audio
