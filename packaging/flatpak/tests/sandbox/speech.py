#!/usr/bin/env python3
"""Drive /app/librust_tts_wrapper.so through its C API, inside the sandbox.

Run by smoke-test.py inside `flatpak build`, never on the host. The library
is the one the app links (src/Output/TtsService.cpp calls the same
functions), loaded here with ctypes so that no compiler is needed and the
Platform runtime is enough. Each mode prints one JSON object on stdout.

  speech.py info MODEL_ID
      Engines compiled in, and the catalogue entry of MODEL_ID. No network,
      no model on disk.
  speech.py install MODEL_ID
      Download and install MODEL_ID with the wrapper's own installer
      (network). The models land under $HOME/.rust-tts-wrapper, the
      pronunciation data under $VOICEGARDEN_LEXICON_DIR.
  speech.py synth MODEL_ID OUT.wav TEXT
      Synthesize TEXT with the installed MODEL_ID into a WAV file. No
      network; the engine is created the way the app does it.
"""

import ctypes
import json
import sys
import time

LIBRARY = "/app/librust_tts_wrapper.so"
ENGINE = b"sherpaonnx"


class EngineInfo(ctypes.Structure):
    # struct tts_engine_info in rust-tts-wrapper/include/tts_wrapper.h
    _fields_ = [
        ("id", ctypes.c_char_p),
        ("name", ctypes.c_char_p),
        ("needs_credentials", ctypes.c_bool),
        ("credential_keys_json", ctypes.c_char_p),
    ]


def load():
    lib = ctypes.CDLL(LIBRARY)
    lib.tts_get_engines.argtypes = [
        ctypes.POINTER(ctypes.POINTER(EngineInfo)),
        ctypes.POINTER(ctypes.c_int32),
    ]
    lib.tts_free_engines.argtypes = [ctypes.POINTER(EngineInfo), ctypes.c_int32]
    lib.tts_get_sherpa_models_json.restype = ctypes.c_void_p
    lib.tts_get_sherpa_catalog_version.restype = ctypes.c_int32
    lib.tts_free_string.argtypes = [ctypes.c_void_p]
    lib.tts_get_last_error.restype = ctypes.c_char_p
    lib.tts_get_last_error.argtypes = [ctypes.c_void_p]
    lib.tts_model_install.restype = ctypes.c_bool
    lib.tts_model_install.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    lib.tts_models_dir.restype = ctypes.c_void_p
    lib.tts_models_dir.argtypes = [ctypes.c_char_p]
    lib.tts_create.restype = ctypes.c_void_p
    lib.tts_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    lib.tts_destroy.argtypes = [ctypes.c_void_p]
    lib.tts_synth_to_wav.restype = ctypes.c_int32
    lib.tts_synth_to_wav.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lib.tts_free_bytes.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
    return lib


def owned_string(lib, pointer):
    """Copy and free a string the library allocated; None for NULL."""
    if not pointer:
        return None
    text = ctypes.string_at(pointer).decode(errors="replace")
    lib.tts_free_string(pointer)
    return text


def last_error(lib, ctx=None):
    message = lib.tts_get_last_error(ctx)
    return message.decode(errors="replace") if message else None


def info(lib, model_id):
    engines = ctypes.POINTER(EngineInfo)()
    count = ctypes.c_int32()
    status = lib.tts_get_engines(ctypes.byref(engines), ctypes.byref(count))
    ids = [engines[i].id.decode() for i in range(count.value)] if status == 0 else []
    if status == 0:
        lib.tts_free_engines(engines, count)
    catalogue = json.loads(owned_string(lib, lib.tts_get_sherpa_models_json()) or "{}")
    return {
        "engines_status": status,
        "engines": ids,
        "catalogue_version": lib.tts_get_sherpa_catalog_version(),
        "catalogue_size": len(catalogue),
        "model": catalogue.get(model_id),
        "models_dir": owned_string(lib, lib.tts_models_dir(ENGINE)),
    }


def install(lib, model_id):
    started = time.monotonic()
    ok = lib.tts_model_install(ENGINE, model_id.encode(), None, None, None)
    return {
        "installed": bool(ok),
        "seconds": round(time.monotonic() - started, 1),
        "error": None if ok else last_error(lib),
        "models_dir": owned_string(lib, lib.tts_models_dir(ENGINE)),
    }


def synth(lib, model_id, out_path, text):
    # The app selects a Sherpa voice with exactly this credentials JSON
    # (src/Preferences/PreferencesWindow.cpp).
    started = time.monotonic()
    ctx = lib.tts_create(ENGINE, json.dumps({"modelId": model_id}).encode())
    if not ctx:
        return {"created": False, "error": last_error(lib)}
    created = round(time.monotonic() - started, 2)
    data = ctypes.POINTER(ctypes.c_uint8)()
    length = ctypes.c_size_t()
    started = time.monotonic()
    status = lib.tts_synth_to_wav(ctx, text.encode(), ctypes.byref(data), ctypes.byref(length))
    result = {
        "created": True,
        "create_seconds": created,
        "synth_status": status,
        "synth_seconds": round(time.monotonic() - started, 2),
        "bytes": length.value if status == 0 else 0,
        "error": None if status == 0 else last_error(lib, ctx),
    }
    if status == 0:
        with open(out_path, "wb") as f:
            f.write(ctypes.string_at(data, length.value))
        lib.tts_free_bytes(data, length)
    lib.tts_destroy(ctx)
    return result


def main():
    mode, model_id = sys.argv[1], sys.argv[2]
    try:
        lib = load()
    except OSError as e:
        print(json.dumps({"load_error": str(e)}))
        return 0
    if mode == "info":
        result = info(lib, model_id)
    elif mode == "install":
        result = install(lib, model_id)
    elif mode == "synth":
        result = synth(lib, model_id, sys.argv[3], sys.argv[4])
    else:
        raise SystemExit(f"unknown mode {mode!r}")
    sys.stdout.flush()
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
