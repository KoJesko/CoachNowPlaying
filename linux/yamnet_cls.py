# SPDX-License-Identifier: MIT
"""Speech / music check for coach-mpris-bridge with YAMNet (AudioSet classes), ONNX export by Qualcomm AI Hub
(https://huggingface.co/qualcomm/YamNet, MIT). Input: mono float32 audio at 16 kHz."""
import numpy as np, os

SR, WIN, HOP, NFFT, NMEL, FMIN, FMAX, PATCH = 16000, 400, 160, 512, 64, 125.0, 7500.0, 96

def _hz2mel(f): return 1127.0 * np.log1p(np.asarray(f) / 700.0)  # HTK, as tf.signal.linear_to_mel_weight_matrix

def _melmat():
    bins = np.linspace(0, SR / 2, NFFT // 2 + 1)
    edges = np.linspace(_hz2mel(FMIN), _hz2mel(FMAX), NMEL + 2)
    m = _hz2mel(bins)[:, None]
    lo, ce, hi = edges[:-2][None], edges[1:-1][None], edges[2:][None]
    w = np.maximum(0, np.minimum((m - lo) / (ce - lo), (hi - m) / (hi - ce)))
    w[0, :] = 0  # tf drops the DC bin
    return w.astype(np.float32)

MEL = _melmat()
WINDOW = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(WIN) / WIN)).astype(np.float32)  # periodic Hann

def patches(x):
    n = 1 + (len(x) - WIN) // HOP
    idx = np.arange(WIN)[None] + HOP * np.arange(n)[:, None]
    spec = np.abs(np.fft.rfft(x[idx] * WINDOW, NFFT)).astype(np.float32)
    logmel = np.log(spec @ MEL + 0.001)
    return [logmel[s:s + PATCH] for s in range(0, len(logmel) - PATCH + 1, PATCH // 2)]

class Yamnet:
    def __init__(self, model_dir):
        import onnxruntime as ort
        self.s = ort.InferenceSession(os.path.join(model_dir, "yamnet.onnx"), providers=["CPUExecutionProvider"])
        self.labels = [l.strip() for l in open(os.path.join(model_dir, "labels.txt"), encoding="utf-8")]
    def scores(self, x16k):
        ps = patches(x16k)
        if not ps: return None
        out = [self.s.run(None, {"audio": p[None, None].astype(np.float32)})[0][0] for p in ps]
        return np.mean(out, axis=0)


SPEECH = ("Speech", "Narration, monologue", "Conversation", "Male speech, man speaking", "Female speech, woman speaking")


def is_speech(model, x16k):
    """True if the clip is mostly talk, False if mostly music, None if unsure."""
    s = model.scores(x16k)
    if s is None:
        return None
    ix = {l: i for i, l in enumerate(model.labels)}
    speech = max(s[ix[k]] for k in SPEECH if k in ix)
    music = s[ix["Music"]] if "Music" in ix else -99.0
    if abs(speech - music) < 2.0:
        return None
    return bool(speech > music)
