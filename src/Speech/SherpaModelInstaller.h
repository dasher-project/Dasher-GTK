// Downloads, verifies and installs Sherpa-ONNX models from the
// registry into ~/.rust-tts-wrapper/sherpaonnx — the GTK counterpart
// of Dasher-Apple's SpeechModelStorage:
//
//   - GitHub tarballs (.tar.bz2) verified against the registry sha256
//     (plus pinned replacement digests for post-registry re-uploads)
//   - MMS voices fetched per-file from Hugging Face (LFS sha256, or
//     git-blob sha1 for small plain files)
//   - English-only voices without a bundled lexicon get the pinned
//     k2-fsa en-US lexicon; matcha/zipvoice get their vocoder
//   - needs_bundle voices get their voicegarden g2p bundle via the
//     wrapper's provisioner (injected, keeping this file FFI-free)
//   - tar extraction skips espeak-ng-data entirely (no-espeak policy;
//     the lexicon is the only phonetic front-end) and refuses path
//     traversal
//
// All work runs on a caller-supplied thread context: methods block,
// report progress through a callback, and poll a cancel flag at every
// chunk boundary.
#pragma once

#include "SherpaModels.h"

#include <functional>
#include <optional>
#include <string>

namespace Dasher {

class SherpaModelInstaller {
  public:
    // Progress callback: fraction in [0,1] (negative when the total is
    // unknown) plus a short human stage label ("Downloading…").
    using ProgressFn = std::function<void(double fraction, const std::string& stage)>;
    using CancelFn = std::function<bool()>;
    // Provisions the voicegarden g2p bundle for a model id; returns
    // true when the bundle is cached afterwards. Wired to
    // tts_provision_sherpa_g2p_bundle by the caller.
    using G2pProvisioner = std::function<bool(const std::string& model_id)>;

    explicit SherpaModelInstaller(G2pProvisioner provision_g2p = {});

    // Blocking install; false fills `error` with a user-presentable
    // reason. Checks download_unavailable_reason() first, so callers
    // that already gate on it simply never see those errors here.
    bool install(const SherpaModel& model, const CancelFn& cancelled, const ProgressFn& progress, std::string& error);

  private:
    G2pProvisioner m_provision_g2p;
};

} // namespace Dasher
