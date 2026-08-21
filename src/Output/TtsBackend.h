#pragma once

// Selects which TTS implementation the UI talks to. Both present the same
// interface, so everything above this line is backend-agnostic.
//
//   -DDASHER_TTS_BACKEND=wrapper  (default)  link rust-tts-wrapper via its C FFI
//   -DDASHER_TTS_BACKEND=ssip                talk SSIP to speech-dispatcher
//
// The ssip backend is the spike for dasher-project/Dasher-GTK#47: it needs no
// Rust toolchain, no cargo step, and no FFI, because the engines live in a
// speech-dispatcher output module in another process.

#ifdef DASHER_TTS_SSIP
#include "Output/SsipTtsService.h"
using TtsBackend = SsipTtsService;
#else
#include "Output/TtsService.h"
using TtsBackend = TtsService;
#endif
