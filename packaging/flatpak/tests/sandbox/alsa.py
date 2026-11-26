#!/usr/bin/env python3
"""Open and close the default ALSA playback device, inside the sandbox.

Run by smoke-test.py inside `flatpak build` with the PulseAudio socket
shared. The app's in-process player (rust-tts-wrapper `speaker`, cpal on
ALSA) opens the same device; in the runtime "default" is the PulseAudio
plugin, which needs the sound server socket. Nothing is played: the device
is opened and closed. Prints one JSON object.
"""

import ctypes
import json

SND_PCM_STREAM_PLAYBACK = 0


def main():
    alsa = ctypes.CDLL("libasound.so.2")
    alsa.snd_strerror.restype = ctypes.c_char_p
    pcm = ctypes.c_void_p()
    status = alsa.snd_pcm_open(ctypes.byref(pcm), b"default", SND_PCM_STREAM_PLAYBACK, 0)
    result = {"status": status, "error": None}
    if status == 0:
        alsa.snd_pcm_close(pcm)
    else:
        result["error"] = alsa.snd_strerror(status).decode(errors="replace")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
