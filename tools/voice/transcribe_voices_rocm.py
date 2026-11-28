# SPDX-License-Identifier: GPL-3.0-or-later
r"""GPU-accelerated dual-source (JP + EN) Whisper transcriber for XC2 voice manifests (ROCm/CUDA).

Optimized direct-batch implementation:
  - Decodes Wwise 0x3041 Opus .wem streams in memory via extract_wwise_voices.wem_opus_to_ogg + PyAV
    (zero temp WAVs, zero external ffmpeg) using a background thread pool.
  - Bypasses transformers.pipeline() overhead and calls model.generate() directly under
    torch.inference_mode() with fp16, SDPA attention, max_new_tokens=96, and large batches.
  - Resumable: skips events already present in --out JSONL and flushes every batch.

Example (any CUDA/ROCm PyTorch environment):
    python tools\voice\transcribe_voices_rocm.py \
        --romfs <path-to-romfs> \
        --manifest work\voice_pipeline\manifest.json \
        --out work\voice_pipeline\transcripts.jsonl \
        --tiers banter banter_orphan high \
        --batch-size 4 --max-new-tokens 96
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import io
import json
import pathlib
import sys
import time

import av
import numpy as np
import torch
from transformers import AutoModelForSpeechSeq2Seq, AutoProcessor

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import extract_wwise_voices as ewv


def decode_ogg_to_16k(ogg_bytes: bytes) -> np.ndarray:
    container = av.open(io.BytesIO(ogg_bytes))
    resampler = av.audio.resampler.AudioResampler(format="fltp", layout="mono", rate=16000)
    chunks = []
    for frame in container.decode(audio=0):
        for rf in resampler.resample(frame):
            chunks.append(rf.to_ndarray()[0])
    if not chunks:
        return np.zeros(0, dtype=np.float32)
    return np.concatenate(chunks).astype(np.float32, copy=False)


def load_pck_wems(pck_path: pathlib.Path):
    pck = pck_path.read_bytes()
    _, wems = ewv.parse_akpk(pck)
    return pck, wems


def extract_pcm(pck: bytes, wems: dict, wem_id: int) -> np.ndarray | None:
    if not wem_id or wem_id not in wems:
        return None
    woff, wsz, _ = wems[wem_id]
    raw = pck[woff : woff + wsz]
    if raw[:4] != b"RIFF":
        return None
    try:
        ogg, _ = ewv.wem_opus_to_ogg(raw)
        return decode_ogg_to_16k(ogg)
    except Exception:
        return None


def looks_suspicious(text: str, dur: float) -> bool:
    t = text.strip()
    if not t:
        return dur >= 0.8
    if dur >= 2.0 and len(t) <= 2:
        return True
    for bad in ("ご視聴ありがとうございました", "チャンネル登録", "Thank you for watching", "Subtitles by"):
        if bad.lower() in t.lower():
            return True
    return False


def transcribe_batch(model, proc, arrays: list[np.ndarray], lang: str, device: str,
                     dtype: torch.dtype, max_new_tokens: int) -> list[str]:
    if not arrays:
        return []
    feats = proc.feature_extractor(
        arrays,
        sampling_rate=16000,
        return_tensors="pt",
        return_attention_mask=True,
    )
    input_features = feats.input_features.to(device=device, dtype=dtype, non_blocking=True)
    attention_mask = feats.attention_mask.to(device=device, non_blocking=True)
    with torch.inference_mode():
        pred_ids = model.generate(
            input_features=input_features,
            attention_mask=attention_mask,
            language=lang,
            task="transcribe",
            max_new_tokens=max_new_tokens,
            num_beams=1,
            do_sample=False,
            use_cache=True,
        )
    texts = proc.tokenizer.batch_decode(pred_ids, skip_special_tokens=True)
    return [t.strip() for t in texts]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--out", required=True, help="Output JSONL path (resumable)")
    ap.add_argument("--model", default="openai/whisper-large-v3-turbo")
    ap.add_argument("--tiers", nargs="+", default=["banter", "banter_orphan", "high"])
    ap.add_argument("--packs", nargs="+", default=["main", "ira"])
    ap.add_argument("--speakers", nargs="*", type=int, default=None,
                    help="Optional filter to specific speaker_ids")
    ap.add_argument("--batch-size", type=int, default=4)
    ap.add_argument("--max-new-tokens", type=int, default=96)
    ap.add_argument("--log-every", type=int, default=32)
    ap.add_argument("--limit", type=int, default=0, help="Optional max clips to process")
    ap.add_argument("--local-only", action="store_true")
    args = ap.parse_args()

    manifest = json.loads(pathlib.Path(args.manifest).read_text(encoding="utf-8"))
    out_path = pathlib.Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    done: set[str] = set()
    if out_path.exists():
        for line in out_path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                done.add(json.loads(line)["event"])
            except Exception:
                pass

    tier_set = set(args.tiers)
    pack_set = set(args.packs)
    sp_set = set(args.speakers) if args.speakers else None

    todo = [
        e for e in manifest["entries"]
        if e["tier"] in tier_set
        and e["pack"] in pack_set
        and (sp_set is None or e["speaker_id"] in sp_set)
        and e["event"] not in done
    ]
    # Process banter first, then main cast/Torna/Rare/DLC (non-arts before arts KK=39), then common blades (61..80)
    tier_rank = {"banter": 0, "banter_orphan": 1, "high": 2}
    main_core = set(range(1, 22)) | set(range(701, 710))
    common_blades = set(range(61, 81))
    todo.sort(key=lambda e: (
        tier_rank.get(e["tier"], 9),
        1 if e["speaker_id"] in common_blades else 0,
        1 if e["kk"] == 39 else 0,
        0 if e["speaker_id"] in main_core else 1,
        e["speaker_id"],
        e["event"],
    ))
    if args.limit > 0:
        todo = todo[: args.limit]

    print(f"Already done: {len(done)}, remaining todo: {len(todo)}")
    if not todo:
        return

    snd = pathlib.Path(args.romfs) / "sound"
    pcks = {}
    need_packs = {e["pack"] for e in todo}
    if "main" in need_packs:
        pcks[("main", "jp")] = load_pck_wems(snd / "jp.pck")
        pcks[("main", "en")] = load_pck_wems(snd / "en.pck")
    if "ira" in need_packs:
        pcks[("ira", "jp")] = load_pck_wems(snd / "jp_ira.pck")
        pcks[("ira", "en")] = load_pck_wems(snd / "en_ira.pck")

    device = "cuda:0" if torch.cuda.is_available() else "cpu"
    dtype = torch.float16 if device.startswith("cuda") else torch.float32
    dev_name = torch.cuda.get_device_name(0) if device.startswith("cuda") else "CPU"
    print(f"Loading {args.model} on {dev_name} ({dtype}, batch_size={args.batch_size}, sdpa)...")
    t_load = time.time()
    model = AutoModelForSpeechSeq2Seq.from_pretrained(
        args.model,
        dtype=dtype,
        attn_implementation="sdpa",
        local_files_only=args.local_only,
    ).to(device)
    model.eval()
    model.generation_config.max_length = None
    proc = AutoProcessor.from_pretrained(args.model, local_files_only=args.local_only)
    print(f"Model ready in {time.time() - t_load:.1f}s")

    def decode_chunk(chunk: list[dict]):
        jp_arrays, jp_idx = [], []
        en_arrays, en_idx = [], []
        for i, e in enumerate(chunk):
            pck_jp, wems_jp = pcks[(e["pack"], "jp")]
            arr_jp = extract_pcm(pck_jp, wems_jp, e["jp_wem"])
            if arr_jp is not None and len(arr_jp) > 0:
                jp_arrays.append(arr_jp)
                jp_idx.append(i)
            pck_en, wems_en = pcks[(e["pack"], "en")]
            arr_en = extract_pcm(pck_en, wems_en, e["en_wem"])
            if arr_en is not None and len(arr_en) > 0:
                en_arrays.append(arr_en)
                en_idx.append(i)
        return jp_arrays, jp_idx, en_arrays, en_idx

    bs = args.batch_size
    chunks = [todo[s : s + bs] for s in range(0, len(todo), bs)]

    t0 = time.time()
    processed = 0
    flagged = 0
    with ThreadPoolExecutor(max_workers=2) as pool, out_path.open("a", encoding="utf-8") as fout:
        next_fut = pool.submit(decode_chunk, chunks[0])
        for ci, chunk in enumerate(chunks):
            jp_arrays, jp_idx, en_arrays, en_idx = next_fut.result()
            if ci + 1 < len(chunks):
                next_fut = pool.submit(decode_chunk, chunks[ci + 1])

            jp_texts = [""] * len(chunk)
            en_texts = [""] * len(chunk)
            if jp_arrays:
                res_jp = transcribe_batch(model, proc, jp_arrays, "japanese", device, dtype, args.max_new_tokens)
                for i, t in zip(jp_idx, res_jp):
                    jp_texts[i] = t
            if en_arrays:
                res_en = transcribe_batch(model, proc, en_arrays, "english", device, dtype, args.max_new_tokens)
                for i, t in zip(en_idx, res_en):
                    en_texts[i] = t

            for e, tj, te in zip(chunk, jp_texts, en_texts):
                needs_review = looks_suspicious(tj, e["jp_dur"]) or looks_suspicious(te, e["en_dur"])
                if needs_review:
                    flagged += 1
                rec = {
                    "event": e["event"],
                    "pack": e["pack"],
                    "speaker_id": e["speaker_id"],
                    "kk": e["kk"],
                    "tier": e["tier"],
                    "jp_dur": e["jp_dur"],
                    "en_dur": e["en_dur"],
                    "jp_text": tj,
                    "en_text": te,
                    "needs_audio_review": needs_review,
                }
                fout.write(json.dumps(rec, ensure_ascii=False) + "\n")
            fout.flush()
            processed += len(chunk)
            if processed % args.log_every < len(chunk) or processed == len(todo):
                elapsed = time.time() - t0
                rate = processed / max(elapsed, 0.01)
                eta = (len(todo) - processed) / max(rate, 0.01)
                print(f"[{processed}/{len(todo)}] {rate:.2f} clips/s (dual JP+EN), flagged={flagged}, ETA={eta:.0f}s")

    print(f"Done! {processed} clips transcribed in {time.time() - t0:.1f}s (flagged={flagged}).")


if __name__ == "__main__":
    main()
