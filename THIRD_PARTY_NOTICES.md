# 他のソフト・データのライセンス（THIRD_PARTY_NOTICES）

imgdupe そのもの（このリポジトリのソースコード・文書・`imgdupe/data/tags_ja.tsv` の日本語の訳）は、[LICENSE](LICENSE) のとおり **MIT No Attribution（MIT-0）** です。
次のものは imgdupe のものではなく、それぞれの作者のライセンスに従います（使う・配る時は、それぞれのライセンスを確かめてください）。

## exe に入っているもの

| もの | 使い方 | ライセンス |
|---|---|---|
| [SQLite](https://www.sqlite.org/)（`imgdupe/third_party/sqlite`） | DB | パブリックドメイン |
| [hnswlib](https://github.com/nmslib/hnswlib)（`imgdupe/third_party/hnswlib`） | CLIP の近いベクトルを探す索引 | Apache License 2.0（`imgdupe/third_party/hnswlib/LICENSE`） |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) のヘッダー（`imgdupe/third_party/onnxruntime`） | AI のモデルを動かす部品を呼ぶ | MIT（`imgdupe/third_party/onnxruntime/LICENSE`。Copyright (c) Microsoft Corporation） |
| [LAION aesthetic predictor](https://github.com/LAION-AI/aesthetic-predictor) の重み（`imgdupe/src/aesthetic_weights.hpp`） | 見栄えの点数 | MIT |
| WD14 タガーのタグの名前（`imgdupe/data/tags_ja.tsv` の左の列） | タグの日本語の表示名の見出し | 下の WD14 タガーと同じ（Apache License 2.0）。右の列の日本語の訳は imgdupe のもの（MIT-0） |

## 使う時にダウンロードするもの（AI の部品。exe には入っていない）

「その他 → AI の部品」で入れた時に、配布元（または、そこから写したリリースのファイル）からダウンロードします。

| もの | 配布元 | ライセンス（配布元の表示による） |
|---|---|---|
| ONNX Runtime（DirectML） `onnxruntime.dll` | NuGet `Microsoft.ML.OnnxRuntime.DirectML` | MIT |
| DirectML `DirectML.dll` | NuGet `Microsoft.AI.DirectML` | Microsoft の DirectML のライセンス（再配布の条件つき） |
| WD14 タガー（wd-eva02-large-tagger-v3） | Hugging Face `SmilingWolf/wd-eva02-large-tagger-v3` | Apache License 2.0 |
| CLIP の画像の部分（clip-vit-base-patch32） | Hugging Face `Xenova/clip-vit-base-patch32`（OpenAI CLIP を ONNX にしたもの） | MIT（OpenAI CLIP） |
| CLIP の言葉の部分（多言語） | Hugging Face `sentence-transformers/clip-ViT-B-32-multilingual-v1` | Apache License 2.0 |

AI の部品の版は `imgdupe/models.src.json` にあります。部品を足したり替えたりした時は、この表も直してください。
