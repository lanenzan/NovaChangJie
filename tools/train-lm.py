"""Trains NovaChangJie's neural sentence scorer: a small character-level LSTM
language model, exported to ONNX for the NovaChangJieLM helper.

    python tools/train-lm.py [--corpus data/corpus] [--out data/lm] [--epochs 4]

Needs PyTorch (a CUDA GPU makes it much faster), `onnx` and `onnxruntime`
(for 8-bit quantization); the text comes from tools/prepare-corpus.py.
Output:

    nova-lm.onnx     8-bit quantized model (nova-lm.fp32.onnx: unquantized)
                     inputs  ids int64 [batch, time] (id 1 = sentence start
                             first, 0 = padding after the end), lengths int64 [batch]
                     output  score float32 [batch]: log P(sentence) in nats
    nova-lm.vocab    UTF-8, one character per line; line i (from 0) is id i + 3
                     (0 padding, 1 sentence start, 2 unknown character)

The model scores the characters after the start symbol; it does not score
an end symbol, because sentence mode re-scores sentences that are still
being typed.
"""
import argparse
import math
import os
import random
import time

import torch
import torch.nn as nn

PAD, BOS, UNK = 0, 1, 2


class CharLM(nn.Module):
    def __init__(self, vocab, emb=256, hidden=512, layers=2, dropout=0.2):
        super().__init__()
        self.embed = nn.Embedding(vocab, emb, padding_idx=PAD)
        self.lstm = nn.LSTM(emb, hidden, layers, batch_first=True, dropout=dropout)
        self.proj = nn.Linear(hidden, emb)
        self.drop = nn.Dropout(dropout)
        self.out = nn.Linear(emb, vocab, bias=True)
        self.out.weight = self.embed.weight  # Tied input and output embeddings.

    def logits(self, ids):
        h, _ = self.lstm(self.drop(self.embed(ids)))
        return self.out(self.drop(self.proj(h)))

    def forward(self, ids, lengths):
        """Sum of log P(ids[:, t] | ids[:, :t]) for 1 <= t < lengths."""
        logp = torch.log_softmax(self.logits(ids[:, :-1]), dim=-1)
        target = ids[:, 1:]
        tok = logp.gather(2, target.unsqueeze(2)).squeeze(2)
        steps = torch.arange(1, ids.shape[1], device=ids.device).unsqueeze(0)
        mask = (steps < lengths.unsqueeze(1)).to(tok.dtype)
        return (tok * mask).sum(dim=1)


def read_lines(path):
    with open(path, encoding="utf-8") as f:
        return [l.strip() for l in f if l.strip()]


def encode(line, index):
    return [BOS] + [index.get(c, UNK) for c in line]


def batches(seqs, batch_size, shuffle=True):
    order = list(range(len(seqs)))
    if shuffle:
        random.shuffle(order)
    # Group similar lengths to waste less padding.
    chunks = [order[i:i + batch_size * 50] for i in range(0, len(order), batch_size * 50)]
    out = []
    for chunk in chunks:
        chunk.sort(key=lambda i: len(seqs[i]))
        out += [chunk[i:i + batch_size] for i in range(0, len(chunk), batch_size)]
    if shuffle:
        random.shuffle(out)
    return out


def tensorize(seqs, idx, device):
    width = max(len(seqs[i]) for i in idx)
    ids = torch.full((len(idx), width), PAD, dtype=torch.long)
    lengths = torch.empty(len(idx), dtype=torch.long)
    for row, i in enumerate(idx):
        ids[row, : len(seqs[i])] = torch.tensor(seqs[i])
        lengths[row] = len(seqs[i])
    return ids.to(device), lengths.to(device)


def evaluate(model, seqs, device, batch_size=512):
    model.eval()
    total, chars = 0.0, 0
    with torch.no_grad():
        for idx in batches(seqs, batch_size, shuffle=False):
            ids, lengths = tensorize(seqs, idx, device)
            total += -model(ids, lengths).sum().item()
            chars += (lengths - 1).sum().item()
    model.train()
    return math.exp(total / max(chars, 1))


def main():
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", default=os.path.join(root, "data", "corpus"))
    ap.add_argument("--out", default=os.path.join(root, "data", "lm"))
    ap.add_argument("--epochs", type=int, default=4)
    ap.add_argument("--everyday-repeat", type=int, default=3, help="copies of train.txt per epoch")
    ap.add_argument("--law-share", type=float, default=0.3, help="share of laws.txt used per epoch")
    ap.add_argument("--lit-share", type=float, default=0.3, help="share of literature.txt used per epoch")
    ap.add_argument("--min-count", type=int, default=2, help="rarer characters become 'unknown'")
    ap.add_argument("--emb", type=int, default=256)
    ap.add_argument("--hidden", type=int, default=512)
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    random.seed(args.seed)
    torch.manual_seed(args.seed)
    device = "cuda" if torch.cuda.is_available() else "cpu"

    everyday = read_lines(os.path.join(args.corpus, "train.txt"))
    laws = read_lines(os.path.join(args.corpus, "laws.txt"))
    heldout = read_lines(os.path.join(args.corpus, "heldout.txt"))
    lit_path = os.path.join(args.corpus, "literature.txt")
    literature = read_lines(lit_path) if os.path.exists(lit_path) else []

    counts = {}
    for line in everyday * args.everyday_repeat + laws + literature:
        for c in line:
            counts[c] = counts.get(c, 0) + 1
    chars = sorted((c for c, n in counts.items() if n >= args.min_count), key=lambda c: (-counts[c], c))
    index = {c: i + 3 for i, c in enumerate(chars)}
    vocab = len(chars) + 3
    print(f"vocabulary {vocab}, everyday {len(everyday)} lines, laws {len(laws)} lines, "
          f"literature {len(literature)} lines, device {device}", flush=True)

    every_ids = [encode(l, index) for l in everyday]
    law_ids = [encode(l, index) for l in laws]
    lit_ids = [encode(l, index) for l in literature]
    held_ids = [encode(l, index) for l in heldout]

    model = CharLM(vocab, args.emb, args.hidden).to(device)
    print(f"parameters {sum(p.numel() for p in model.parameters()) / 1e6:.1f}M", flush=True)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=0.01)
    per_epoch = (len(every_ids) * args.everyday_repeat + int(len(law_ids) * args.law_share)
                 + int(len(lit_ids) * args.lit_share))
    steps_per_epoch = per_epoch // args.batch + 1
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=steps_per_epoch * args.epochs + 10)

    for epoch in range(args.epochs):
        data = (every_ids * args.everyday_repeat + random.sample(law_ids, int(len(law_ids) * args.law_share))
                + random.sample(lit_ids, int(len(lit_ids) * args.lit_share)))
        start, seen = time.time(), 0
        for idx in batches(data, args.batch):
            ids, lengths = tensorize(data, idx, device)
            loss = -model(ids, lengths).sum() / (lengths - 1).sum()
            opt.zero_grad()
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            sched.step()
            seen += len(idx)
        ppl = evaluate(model, held_ids, device)
        print(f"epoch {epoch + 1}: {seen} lines in {time.time() - start:.0f}s, heldout perplexity {ppl:.1f}", flush=True)

    os.makedirs(args.out, exist_ok=True)
    model.eval().cpu()
    torch.save({"state": model.state_dict(), "vocab": chars, "emb": args.emb, "hidden": args.hidden},
               os.path.join(args.out, "nova-lm.pt"))
    with open(os.path.join(args.out, "nova-lm.vocab"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(chars) + "\n")
    ids = torch.tensor([[BOS, 3, 4, 5], [BOS, 6, 7, PAD]], dtype=torch.long)
    lengths = torch.tensor([4, 3], dtype=torch.long)
    fp32 = os.path.join(args.out, "nova-lm.fp32.onnx")
    torch.onnx.export(model, (ids, lengths), fp32,
                      input_names=["ids", "lengths"], output_names=["score"],
                      dynamic_axes={"ids": {0: "batch", 1: "time"}, "lengths": {0: "batch"}, "score": {0: "batch"}},
                      opset_version=17, dynamo=False)
    # 8-bit weights: a quarter of the size, same accuracy in ncj-eval.
    from onnxruntime.quantization import QuantType, quantize_dynamic
    quantize_dynamic(fp32, os.path.join(args.out, "nova-lm.onnx"), weight_type=QuantType.QInt8)
    print(f"wrote {args.out}", flush=True)


if __name__ == "__main__":
    main()
