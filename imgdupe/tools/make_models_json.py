#!/usr/bin/env python3
"""models.src.json から、リリースに添付する models.json を作る。

  make_models_json.py <models.src.json> <出力フォルダ> <owner/repo> <タグ>

- nuget の部品: パッケージを取ってきて中のファイルを取り出し、出力フォルダに置く（リリースに添付する）。
  URL は https://github.com/<owner/repo>/releases/download/<タグ>/<ファイル名>
- huggingface の部品: revision をコミットに固定し、LFS のファイルは API の sha256 を、それ以外は取ってきて計算する。
  URL は https://huggingface.co/<repo>/resolve/<コミット>/<ファイル>。版はコミットの先頭 12 文字
出力フォルダに models.json と、添付するファイルを書く。
"""
import hashlib
import io
import json
import os
import sys
import time
import urllib.request
import zipfile

UA = {"User-Agent": "imgdupe-release"}


def fetch(url, tries=4):
    for i in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r:
                return r.read()
        except Exception as e:  # noqa: BLE001  通信の失敗は何度か試す
            if i == tries - 1:
                raise RuntimeError(f"{url}: {e}") from e
            time.sleep(5 * (i + 1))


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def nuget_component(c, out_dir, repo, tag):
    files, versions = [], []
    for pkg in c["nuget"]:
        name, ver = pkg["package"], pkg["version"]
        versions.append(ver)
        url = f"https://api.nuget.org/v3-flatcontainer/{name.lower()}/{ver}/{name.lower()}.{ver}.nupkg"
        z = zipfile.ZipFile(io.BytesIO(fetch(url)))
        for inner, dest in pkg["files"].items():
            data = z.read(inner)
            asset = os.path.basename(dest)
            with open(os.path.join(out_dir, asset), "wb") as f:
                f.write(data)
            files.append({"path": dest, "url": f"https://github.com/{repo}/releases/download/{tag}/{asset}",
                          "sha256": sha256(data), "size": len(data)})
    return "+".join(versions), files


def hf_component(c):
    # 1 つのリポジトリ（オブジェクト）か、いくつかのリポジトリ（配列）。版はそれぞれのコミットをつなげたもの
    sources = c["huggingface"] if isinstance(c["huggingface"], list) else [c["huggingface"]]
    versions, files = [], []
    for h in sources:
        v, f = hf_files(h)
        versions.append(v)
        files += f
    return "+".join(versions), files


def hf_files(h):
    repo, rev = h["repo"], h.get("revision", "main")
    commit = json.loads(fetch(f"https://huggingface.co/api/models/{repo}/revision/{rev}"))["sha"]
    tree = json.loads(fetch(f"https://huggingface.co/api/models/{repo}/tree/{commit}?recursive=true"))
    by_path = {e["path"]: e for e in tree if e.get("type") == "file"}
    files = []
    for src, dest in h["files"].items():
        e = by_path.get(src)
        if e is None:
            raise RuntimeError(f"{repo}@{commit} に {src} がありません")
        url = f"https://huggingface.co/{repo}/resolve/{commit}/{src}"
        lfs = e.get("lfs")
        if lfs and len(lfs.get("oid", "")) == 64:
            digest, size = lfs["oid"], lfs["size"]
        else:
            data = fetch(url)
            digest, size = sha256(data), len(data)
        files.append({"path": dest, "url": url, "sha256": digest, "size": size})
    return commit[:12], files


def build(src, out_dir, repo, tag):
    os.makedirs(out_dir, exist_ok=True)
    comps = []
    for c in src["components"]:
        if "nuget" in c:
            version, files = nuget_component(c, out_dir, repo, tag)
        elif "huggingface" in c:
            version, files = hf_component(c)
        else:
            raise RuntimeError(f"{c['id']}: nuget か huggingface を書いてください")
        comps.append({"id": c["id"], "name": c.get("name", c["id"]), "description": c.get("description", ""),
                      "version": c.get("version", version), "requires": c.get("requires", []), "files": files})
    return {"version": 1, "components": comps}


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    src_path, out_dir, repo, tag = sys.argv[1:]
    with open(src_path, encoding="utf-8") as f:
        src = json.load(f)
    manifest = build(src, out_dir, repo, tag)
    with open(os.path.join(out_dir, "models.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1)
    for c in manifest["components"]:
        print(f"{c['id']} {c['version']} {sum(x['size'] for x in c['files']):,} bytes")


if __name__ == "__main__":
    main()
