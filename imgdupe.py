#!/usr/bin/env python3
"""
imgdupe.py - 見た目が同じ画像を探す軽量ツール（読み取り専用・ファイルは一切変更しません）

  完全一致 : ファイルサイズが同じものだけ SHA-256 を計算して比較
  見た目一致: dHash(64bit) のハミング距離で比較（縮小・再圧縮・形式違いも検出）
  キャッシュ: SQLite に保存。2回目以降は新規・変更ファイルだけ計算

使い方:
  python imgdupe.py D:\\Photos                    # 全体から重複グループを探す
  python imgdupe.py D:\\Photos -t 6               # 類似のしきい値（0=ほぼ同一 / 10=ゆるい）
  python imgdupe.py D:\\Photos --exact-only       # 完全一致だけ
  python imgdupe.py D:\\Photos -q sample.jpg      # 1枚と似た画像を検索
必要: pip install pillow numpy
"""
import argparse, csv, hashlib, html, os, sqlite3, sys, time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image, ImageOps

Image.MAX_IMAGE_PIXELS = None
EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tif", ".tiff", ".webp", ".jfif"}
POP8 = np.array([bin(i).count("1") for i in range(256)], dtype=np.uint8)


# ---------- ハッシュ計算 ----------
def dhash_file(path):
    """64bit dHash を返す（失敗時 None）"""
    try:
        with Image.open(path) as im:
            im.draft("L", (128, 128))          # JPEGは縮小デコードで高速化
            im = ImageOps.exif_transpose(im)   # 撮影時の回転情報を反映
            im = im.convert("L").resize((9, 8), Image.BILINEAR)
            px = np.asarray(im, dtype=np.int16)
        bits = (px[:, 1:] > px[:, :-1]).flatten()
        return int(np.packbits(bits).view(">u8")[0])
    except Exception:
        return None


def work_dhash(args):
    path, size, mtime = args
    return path, size, mtime, dhash_file(path)


def work_sha(path):
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return path, h.hexdigest()
    except Exception:
        return path, None


def to_signed(u):  # SQLite は符号付き64bitなので変換
    return u - (1 << 64) if u is not None and u >= (1 << 63) else u


def popcount(x):
    if hasattr(np, "bitwise_count"):
        return np.bitwise_count(x)
    return POP8[x.view(np.uint8).reshape(*x.shape, 8)].sum(-1)


# ---------- スキャン ----------
def scan(root):
    out = []
    for dp, _, fns in os.walk(root):
        for fn in fns:
            if os.path.splitext(fn)[1].lower() in EXTS:
                p = os.path.join(dp, fn)
                try:
                    st = os.stat(p)
                    out.append((p, st.st_size, int(st.st_mtime)))
                except OSError:
                    pass
    return out


def update_cache(db, files, workers):
    db.execute("""CREATE TABLE IF NOT EXISTS files(
        path TEXT PRIMARY KEY, size INT, mtime INT, dhash INT, sha TEXT)""")
    cached = {r[0]: (r[1], r[2]) for r in db.execute("SELECT path,size,mtime FROM files")}
    todo = [f for f in files if cached.get(f[0]) != (f[1], f[2])]
    print(f"画像 {len(files):,} 件 / 新規・変更 {len(todo):,} 件をハッシュ化")
    if todo:
        t0, done = time.time(), 0
        with ProcessPoolExecutor(workers) as ex:
            for path, size, mtime, h in ex.map(work_dhash, todo, chunksize=64):
                db.execute("INSERT OR REPLACE INTO files VALUES(?,?,?,?,NULL)",
                           (path, size, mtime, to_signed(h)))
                done += 1
                if done % 2000 == 0:
                    db.commit()
                    print(f"  {done:,}/{len(todo):,}  {done/(time.time()-t0):.0f} 枚/秒")
        db.commit()
    # 消えたファイルをキャッシュから削除
    alive = {f[0] for f in files}
    gone = [p for p in cached if p not in alive]
    db.executemany("DELETE FROM files WHERE path=?", [(p,) for p in gone])
    db.commit()


def fill_sha(db, workers):
    """サイズが重複するファイルだけ SHA-256 を計算"""
    rows = db.execute("""SELECT path FROM files WHERE sha IS NULL AND size IN
        (SELECT size FROM files GROUP BY size HAVING COUNT(*)>1)""").fetchall()
    if rows:
        print(f"同サイズ候補 {len(rows):,} 件の SHA-256 を計算")
        with ProcessPoolExecutor(workers) as ex:
            for path, sha in ex.map(work_sha, [r[0] for r in rows], chunksize=32):
                db.execute("UPDATE files SET sha=? WHERE path=?", (sha, path))
        db.commit()


# ---------- 比較 ----------
def similar_pairs(H, t, block=128):
    """全ペアのハミング距離をブロック単位で計算（10万件で数十秒）"""
    n = len(H)
    for s in range(0, n, block):
        blk = H[s:s + block]
        d = popcount(blk[:, None] ^ H[None, s:])
        ii, jj = np.nonzero(d <= t)
        keep = jj + s > ii + s
        for i, j, dist in zip(ii[keep] + s, jj[keep] + s, d[ii[keep], jj[keep]]):
            yield int(i), int(j), int(dist)


class UF:
    def __init__(s, n): s.p = list(range(n))
    def find(s, x):
        while s.p[x] != x:
            s.p[x] = s.p[s.p[x]]; x = s.p[x]
        return x
    def union(s, a, b): s.p[s.find(a)] = s.find(b)


# ---------- 出力 ----------
def write_reports(groups, out):
    csv_path, html_path = out + ".csv", out + ".html"
    with open(csv_path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["group", "種類", "距離", "サイズ(KB)", "パス"])
        for gi, (kind, items) in enumerate(groups, 1):
            for path, size, dist in items:
                w.writerow([gi, kind, dist, round(size / 1024, 1), path])
    parts = ["<!doctype html><meta charset=utf-8><title>重複画像</title><style>"
             "body{font-family:sans-serif;margin:16px}.g{border-bottom:1px solid #ccc;padding:8px 0}"
             ".i{display:inline-block;width:180px;margin:4px;vertical-align:top;font-size:11px;word-break:break-all}"
             "img{max-width:180px;max-height:180px;display:block}</style>",
             f"<h2>重複・類似グループ {len(groups):,} 件</h2>"]
    for gi, (kind, items) in enumerate(groups, 1):
        parts.append(f"<div class=g><b>#{gi} {kind}（{len(items)}枚）</b><br>")
        for path, size, dist in items:
            uri = html.escape(Path(path).resolve().as_uri())
            parts.append(f"<div class=i><a href='{uri}'><img loading=lazy src='{uri}'></a>"
                         f"距離{dist} / {size//1024}KB<br>{html.escape(path)}</div>")
        parts.append("</div>")
    Path(html_path).write_text("".join(parts), encoding="utf-8")
    print(f"結果: {csv_path}\n      {html_path}（ブラウザで開くとサムネイル付きで確認できます）")


def main():
    ap = argparse.ArgumentParser(description="見た目が同じ画像を探す")
    ap.add_argument("root", help="検索するフォルダ")
    ap.add_argument("-t", "--threshold", type=int, default=5, help="類似しきい値(0-64, 既定5)")
    ap.add_argument("-q", "--query", help="この画像に似た画像を検索")
    ap.add_argument("--exact-only", action="store_true", help="完全一致だけ探す")
    ap.add_argument("--db", default="imgdupe_cache.sqlite", help="キャッシュDB")
    ap.add_argument("-o", "--out", default="imgdupe_result", help="出力ファイル名(拡張子なし)")
    ap.add_argument("-j", "--workers", type=int, default=os.cpu_count())
    a = ap.parse_args()

    db = sqlite3.connect(a.db)
    update_cache(db, scan(a.root), a.workers)
    rows = db.execute("SELECT path,size,dhash,sha FROM files WHERE dhash IS NOT NULL").fetchall()
    bad = db.execute("SELECT COUNT(*) FROM files WHERE dhash IS NULL").fetchone()[0]
    if bad:
        print(f"※ 読めなかった画像 {bad} 件はスキップ")
    H = np.array([r[2] for r in rows], dtype=np.int64).view(np.uint64)

    # 1枚検索モード
    if a.query:
        qh = dhash_file(a.query)
        if qh is None:
            sys.exit("クエリ画像を読めません")
        d = popcount(H ^ np.uint64(qh))
        idx = np.argsort(d)
        hits = [(rows[i][0], rows[i][1], int(d[i])) for i in idx if d[i] <= a.threshold]
        print(f"{len(hits)} 件ヒット")
        for p, s, dist in hits[:50]:
            print(f"  距離{dist:2d}  {p}")
        write_reports([("検索結果", hits)] if hits else [], a.out)
        return

    # 完全一致
    fill_sha(db, a.workers)
    exact = {}
    for p, s, sha in db.execute("SELECT path,size,sha FROM files WHERE sha IS NOT NULL"):
        exact.setdefault(sha, []).append((p, s, 0))
    groups = [("完全一致", v) for v in exact.values() if len(v) > 1]
    print(f"完全一致グループ: {len(groups):,}")

    # 見た目一致
    if not a.exact_only:
        t0 = time.time()
        uf, dist_of = UF(len(rows)), {}
        for i, j, dist in similar_pairs(H, a.threshold):
            uf.union(i, j)
            dist_of[j] = min(dist_of.get(j, 99), dist)
            dist_of[i] = min(dist_of.get(i, 99), dist)
        clusters = {}
        for i in dist_of:
            clusters.setdefault(uf.find(i), []).append(i)
        exact_sets = [{p for p, _, _ in v} for _, v in groups]
        sim = 0
        for members in clusters.values():
            paths = {rows[i][0] for i in members}
            if any(paths == es for es in exact_sets):
                continue  # 完全一致と同じ組み合わせなら重複表示しない
            items = sorted(((rows[i][0], rows[i][1], dist_of[i]) for i in members),
                           key=lambda x: (x[2], -x[1]))
            groups.append(("見た目一致", items))
            sim += 1
        print(f"見た目一致グループ: {sim:,}（比較 {time.time()-t0:.1f} 秒）")

    write_reports(groups, a.out)


if __name__ == "__main__":
    main()
