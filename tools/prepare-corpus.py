"""Downloads permissively licensed Traditional Chinese text and turns it into
the training and test text used for NovaChangJie's language models.

    python tools/prepare-corpus.py [--out data/corpus] [--cache data/corpus/download]

Output (UTF-8, one phrase per line: a run of Han characters between
punctuation, i.e. what is typed as one sentence-mode composition):

    train.txt    everyday training text (minus heldout.txt)
    laws.txt     Taiwan and Hong Kong law text, each phrase once
    heldout.txt  everyday sentences kept out of training, for ncj-eval;
                 heldout-tw.txt / heldout-hk.txt split it by region
    essay-cantonese.txt  Cantonese word list with frequencies (rime-cantonese),
                 for words the dictionary lacks (ncj-dictbuild --extra-words)
    literature.txt  public-domain vernacular (白話) novels and stories,
                 old character forms changed to today's
    heldout-lit.txt  phrases of the books kept out of training (魯迅 吶喊),
                 for ncj-eval
    SOURCES.md   where every file came from and under which licence

Sources (only CC0 or attribution-only licences):

  * Common Voice sentence lists, zh-TW / zh-HK / yue (CC0 1.0), pinned to a
    commit and verified by SHA-256.
  * Taiwan: national laws and regulations, 全國法規資料庫 Open API (Ministry of
    Justice). 政府資料開放授權條款 第1版, compatible with CC BY 4.0; laws are
    also not subject to copyright (著作權法第9條).
  * Hong Kong: Hong Kong e-Legislation, current version, Traditional Chinese
    (Department of Justice via DATA.GOV.HK). DATA.GOV.HK terms: free reuse
    with attribution.
  * Cantonese: the Hong Kong Cantonese Corpus (HKCanCor, conversation and
    radio, CC BY; the copy in PyCantonese, pinned), Tatoeba's Cantonese
    sentences (CC BY 2.0 FR) and rime-cantonese's word list (CC BY 4.0, pinned).
  * Literature: Project Gutenberg's vernacular Chinese books by authors who
    died before 1955, so public domain in Hong Kong, Taiwan and countries
    with life + 70 years. Gutenberg's own header and licence text are
    removed. Old variant forms (偸, 汚, 麽) become today's (偷, 污, 麼)
    using Unicode's Unihan variant data (Unicode License v3, pinned).

The government files and the Tatoeba export are updated by their
publishers, so they are not pinned; their SHA-256 is recorded in SOURCES.md.

Only the standard library is used.
"""
import argparse
import hashlib
import io
import json
import os
import re
import sys
import urllib.request
import zipfile

CV_COMMIT = "7aef2b5b7d712f448d896d10776d8abdf9e53b4e"
CV_BASE = f"https://raw.githubusercontent.com/common-voice/common-voice/{CV_COMMIT}/server/data/"
CV_FILES = {
    "zh-TW/sentence-collector.txt": "c85d4b4cb9f20195c015d4c74c947c520e6bdc9e50c83d75d0eaa77c67cf2e7b",
    "zh-TW/setences.txt": "7d6d591f3ac78b368f7e234c937f06e775d52dfbcf90e54fbb1476891cc046e3",
    "zh-TW/taipei_city_gov.txt": None,
    "zh-TW/chatlogs.txt": None,
    "zh-TW/lms.txt": None,
    "zh-HK/sentence-collector.txt": "0c537e9d65c1f1de10d70a1e0989d929b557628d975bf480d8b3b6fcc6291f26",
    "zh-HK/hkgov.txt": "a9430cc6e46184747b64ccc339efb4f455a0fc3888e23655aadf07d7cc0e9092",
    "zh-HK/mtr.txt": "1ff64c1dd1ae34e93aa445193bd785fe948ab9d122ad787f3813187c799bb257",
    "zh-HK/advertise.txt": None,
    "yue/sentence-collector.txt": "645d6a544634db879cef9d38e13ce58b7cef91772ba4d2ae6f6f674dbeb02a8b",
}
CV_LICENSE = "CC0 1.0 (https://github.com/common-voice/common-voice/blob/main/server/data/LICENSE)"

TW_LAW_URL = "https://law.moj.gov.tw/api/Ch/Law/JSON"
TW_LAW_LICENSE = "政府資料開放授權條款 第1版 (https://data.gov.tw/license), compatible with CC BY 4.0"

HK_LEG_URLS = [
    f"https://resource.data.one.gov.hk/doj/data/hkel_c_{part}_zh-Hant.zip"
    for part in ("leg_cap_1_cap_300", "leg_cap_301_cap_600", "leg_cap_601_cap_end", "instruments")
]
HK_LEG_LICENSE = "DATA.GOV.HK terms and conditions (https://data.gov.hk/en/terms-and-conditions), attribution"

HKCANCOR_COMMIT = "3d729c01e53f6056cb1c6c527953837e9a106147"
HKCANCOR_API = ("https://api.github.com/repos/jacksonllee/pycantonese/contents/src/pycantonese/data/hkcancor"
                f"?ref={HKCANCOR_COMMIT}")
HKCANCOR_LICENSE = ("CC BY (https://github.com/fcbond/hkcancor/blob/master/data/LICENSE); K. K. Luke and May L.Y. Wong "
                    "(2015) The Hong Kong Cantonese Corpus: Design and Uses. Journal of Chinese Linguistics Monograph "
                    "Series, 25, 312-333")

TATOEBA_URL = "https://downloads.tatoeba.org/exports/per_language/yue/yue_sentences.tsv.bz2"
TATOEBA_LICENSE = "CC BY 2.0 FR (https://tatoeba.org/en/terms_of_use), Tatoeba contributors"

RIME_CANTONESE_URL = ("https://raw.githubusercontent.com/rime/rime-cantonese/"
                      "259f0e48bba840c3a2e0d117539e96937f3d89bc/essay-cantonese.txt")
RIME_CANTONESE_SHA = "d08836175f598219f43c2f2f9e12e12711212dbefe08d57b2eddb7a9d9f22a5d"
RIME_CANTONESE_LICENSE = "CC BY 4.0 (https://github.com/rime/rime-cantonese), CanCLID"

GUTENBERG_URL = "https://www.gutenberg.org/cache/epub/{0}/pg{0}.txt"
GUTENBERG_LICENSE = "public domain (Project Gutenberg; authors died before 1955)"
# Gutenberg book number: title. Chosen from the catalog's fiction: vernacular (白話)
# rather than classical (文言) by how often 的了們這那麼 etc. appear, Traditional
# characters, no modern editor's notes.
LIT_BOOKS = {
    4572: "粉妝樓1-10回",
    4573: "粉妝樓11-20回",
    4574: "粉妝樓21-30回",
    4575: "粉妝樓31-40回",
    4576: "粉妝樓41-50回",
    4577: "粉妝樓51-60回",
    4578: "粉妝樓61-70回",
    4580: "粉妝樓",
    23824: "說唐",
    23825: "施公案",
    23850: "老殘遊記",
    23863: "水滸傳",
    23865: "恨海",
    23867: "斬鬼傳",
    23907: "無聲戲",
    23962: "西遊記",
    24027: "醉醒石",
    24029: "冷眼观",
    24032: "儒林外史",
    24042: "徬徨",
    24079: "商界現形記",
    24099: "二十年目睹之怪現狀",
    24113: "九命奇冤",
    24138: "官場現形記",
    24139: "官場現形記",
    24142: "轟天雷",
    24154: "癡人說夢記",
    24162: "二刻拍案惊奇",
    24185: "比目魚",
    24227: "情變",
    24230: "今古奇觀",
    24231: "韓湘子全傳",
    24233: "東度記",
    24239: "醒世恆言",
    24264: "紅樓夢",
    24294: "警悟鐘",
    24327: "何典",
    25124: "老殘遊記",
    25128: "孽海花",
    25134: "九尾狐",
    25137: "玉蟾記",
    25146: "山水情",
    25187: "花月痕",
    25195: "風月鑒",
    25202: "補紅樓夢",
    25217: "水滸後傳",
    25219: "花月痕",
    25226: "負曝閒談",
    25229: "飛龍全傳",
    25242: "野草",
    25248: "平妖傳",
    25251: "八段錦",
    25259: "公墓",
    25260: "豐收",
    25271: "朝花夕拾",
    25279: "錦香亭",
    25286: "歡喜冤家",
    25327: "兒女英雄傳",
    25328: "豆棚閒話",
    25329: "春秋配",
    25346: "南腔北調集",
    25350: "蕩寇志",
    25369: "金石緣",
    25376: "三俠五義",
    25379: "文明小史",
    25393: "施公案",
    25402: "九命奇冤",
    25543: "蜃樓志",
    25561: "癡人說夢記",
    25602: "蘭花夢奇傳",
    25649: "巧聯珠",
    26161: "醒世姻緣",
    26729: "二刻拍案驚奇",
    26739: "雪月梅傳",
    26748: "幻中游",
    26871: "粉妝樓全傳",
    26931: "風月夢",
    26970: "彭公案",
    27104: "歸蓮夢",
    27119: "比目魚",
    27148: "蕉葉帕",
    27216: "斬鬼傳",
    27302: "鄰女語",
    27331: "飛跎全傳",
    27402: "風流悟",
    27415: "殺子報",
    27636: "沉沦",
    27686: "狄公案",
    27753: "一枕奇",
    27768: "瞎騙奇聞",
    52200: "金瓶梅",
    52205: "肉蒲團",
    54756: "官場現形記",
    56291: "老殘遊記續集",
    57227: "平妖傳",
    57248: "初刻拍案驚奇",
}
# Kept out of training for heldout-lit.txt: 魯迅's 吶喊 (with 孔乙己), and the
# stories from it that Gutenberg also has as separate books.
LIT_HELDOUT = {27166: "吶喊", 25332: "阿Q正傳", 25423: "狂人日記", 25297: "狂人日記"}
UNIHAN_URL = "https://www.unicode.org/Public/16.0.0/ucd/Unihan.zip"
UNIHAN_SHA = "b8f000df69de7828d21326a2ffea462b04bc7560022989f7cc704f10521ef3e0"
UNIHAN_LICENSE = "Unicode License v3 (https://www.unicode.org/license.txt)"

HELDOUT_PERCENT = 10  # Of the Common Voice phrases.
MAX_PHRASE = 20  # Characters; longer runs are split (the IME commits at 20).


def is_han(ch):
    cp = ord(ch)
    return (
        0x4E00 <= cp <= 0x9FFF
        or 0x3400 <= cp <= 0x4DBF
        or 0xF900 <= cp <= 0xFAFF
        or 0x20000 <= cp <= 0x3134F
        or cp == 0x3007
    )


def phrases(text):
    """Runs of Han characters; anything else (punctuation, digits, Latin) splits."""
    run = []
    for ch in text:
        if is_han(ch):
            run.append(ch)
            if len(run) == MAX_PHRASE:
                yield "".join(run)
                run = []
        else:
            if run:
                yield "".join(run)
            run = []
    if run:
        yield "".join(run)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(url, cache_dir, name, expected=None):
    path = os.path.join(cache_dir, name)
    if os.path.exists(path):
        with open(path, "rb") as f:
            data = f.read()
        if expected is None or sha256(data) == expected:
            return data, path
    print(f"Downloading {url} ...", flush=True)
    req = urllib.request.Request(url, headers={"User-Agent": "NovaChangJie-corpus/1.0"})
    with urllib.request.urlopen(req, timeout=600) as r:
        data = r.read()
    if expected is not None and sha256(data) != expected:
        raise SystemExit(f"checksum mismatch for {url}")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return data, path


def tw_law_text(data):
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        laws = json.loads(z.read("ChLaw.json").decode("utf-8-sig"))["Laws"]
    for law in laws:
        yield law.get("LawName", "")
        yield law.get("LawForeword", "")
        articles = law.get("LawArticles")
        if isinstance(articles, str):
            # Some dumps store the article list as a Python-style string.
            for m in re.finditer(r"'ArticleContent': '((?:[^'\\]|\\.)*)'", articles):
                yield m.group(1).replace("\\r\\n", "\n")
        elif isinstance(articles, list):
            for a in articles:
                yield a.get("ArticleContent", "")


TAG = re.compile(r"<[^>]+>")


def hk_leg_text(data):
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for name in z.namelist():
            if not name.endswith(".xml"):
                continue
            xml = z.read(name).decode("utf-8", errors="replace")
            # Element boundaries split phrases; the <meta> block is skipped.
            xml = re.sub(r"<meta>.*?</meta>", " ", xml, flags=re.S)
            yield TAG.sub("\n", xml)


def hkcancor_text(cache):
    """Utterances of the CHAT files; words are separated by spaces."""
    listing, _ = fetch(HKCANCOR_API, cache, "hkcancor/listing.json")
    names = sorted(x["name"] for x in json.loads(listing) if x["name"].endswith(".cha"))
    digest = hashlib.sha256()
    for name in names:
        data, _ = fetch(f"https://raw.githubusercontent.com/jacksonllee/pycantonese/{HKCANCOR_COMMIT}/"
                        f"src/pycantonese/data/hkcancor/{name}", cache, "hkcancor/" + name)
        digest.update(data)
        for line in data.decode("utf-8").splitlines():
            if line.startswith("*") and ":\t" in line:
                yield line.split(":\t", 1)[1].replace(" ", "")
    hkcancor_text.digest = digest.hexdigest()


def tatoeba_text(data):
    import bz2
    for line in bz2.decompress(data).decode("utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) >= 3:
            yield fields[2]


def gutenberg_body(data):
    """The book text without Project Gutenberg's header and licence."""
    text = data.decode("utf-8", errors="replace").replace("\r\n", "\n")
    start, end = text.find("*** START OF"), text.find("*** END OF")
    if start >= 0 and end > start:
        text = text[text.find("\n", start) + 1:end]
    return text


def variant_map(unihan, table_path, everyday_counts):
    """Characters outside Big5 mapped to the Big5 form everyday text uses:
    simplified characters by the code table's own pairs (开 -> 開), old forms
    by Unihan's variants (偸 -> 偷). Big5 characters, and HKSCS ones that
    everyday text uses (裏, 着, 衆), are never changed. Needs the code table from tools/fetch-data.ps1; without it
    nothing is mapped."""
    if not os.path.exists(table_path):
        print("note: data/sources/table.txt missing, old character forms are kept")
        return {}
    big5, freq, simp = set(), {}, {}
    with open(table_path, encoding="utf-8") as f:
        for line in f:
            fields = line.split()
            if len(fields) < 15 or line.startswith("#"):
                continue
            trad = fields[0]
            if fields[3] == "1" or (fields[4] == "1" and everyday_counts.get(trad, 0) >= 5):
                big5.add(trad)
            if fields[14].isdigit():
                freq[trad] = max(freq.get(trad, 0), int(fields[14]))
            if fields[1] != trad and fields[1] != "NA":
                simp.setdefault(fields[1], []).append(trad)
    out = {}
    for s_char, trads in simp.items():
        if s_char in big5:
            continue
        best = max(trads, key=lambda t: freq.get(t, 0))
        if best in big5 and freq.get(best, 0) > 0:
            out[s_char] = best
    with zipfile.ZipFile(io.BytesIO(unihan)) as z:
        for line in z.read("Unihan_Variants.txt").decode("utf-8").splitlines():
            fields = line.split("\t")
            if len(fields) < 3 or fields[1] not in ("kSemanticVariant", "kZVariant"):
                continue
            src = chr(int(fields[0][2:], 16))
            if src in big5 or src in out:
                continue
            for target in fields[2].split():
                dst = chr(int(target.split("<")[0][2:], 16))
                if dst in big5 and freq.get(dst, 0) > 0 and freq[dst] > freq.get(out.get(src, ""), -1):
                    out[src] = dst
    return out


def main():
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(root, "data", "corpus"))
    ap.add_argument("--cache", default=None, help="download cache (default: <out>/download)")
    ap.add_argument("--no-laws", action="store_true", help="Common Voice only")
    args = ap.parse_args()
    cache = args.cache or os.path.join(args.out, "download")
    os.makedirs(args.out, exist_ok=True)

    notes = ["# NovaChangJie language model text", "",
             "train.txt, laws.txt and heldout.txt (and everything trained from them) are built from:", ""]
    everyday, laws = [], []  # everyday: (phrase, "tw" or "hk")

    for rel, expected in CV_FILES.items():
        data, _ = fetch(CV_BASE + rel, cache, "cv/" + rel, expected)
        region = "tw" if rel.startswith("zh-TW") else "hk"
        for line in data.decode("utf-8").splitlines():
            everyday.extend((p, region) for p in phrases(line))
        notes.append(f"- Common Voice `{rel}` - {CV_BASE + rel}")
        notes.append(f"  - SHA-256: `{sha256(data)}`; License: {CV_LICENSE}")

    for text in hkcancor_text(cache):
        everyday.extend((p, "hk") for p in phrases(text))
    notes.append(f"- Hong Kong Cantonese Corpus (HKCanCor, PyCantonese copy at {HKCANCOR_COMMIT})")
    notes.append(f"  - SHA-256 of the .cha files: `{hkcancor_text.digest}`; License: {HKCANCOR_LICENSE}")
    data, _ = fetch(TATOEBA_URL, cache, "tatoeba/yue_sentences.tsv.bz2")
    for text in tatoeba_text(data):
        everyday.extend((p, "hk") for p in phrases(text))
    notes.append(f"- Tatoeba Cantonese sentences - {TATOEBA_URL}")
    notes.append(f"  - SHA-256: `{sha256(data)}`; License: {TATOEBA_LICENSE}")
    data, _ = fetch(RIME_CANTONESE_URL, cache, "rime-cantonese/essay-cantonese.txt", RIME_CANTONESE_SHA)
    with open(os.path.join(args.out, "essay-cantonese.txt"), "wb") as f:
        f.write(data)
    notes.append(f"- rime-cantonese `essay-cantonese.txt` (word list) - {RIME_CANTONESE_URL}")
    notes.append(f"  - SHA-256: `{RIME_CANTONESE_SHA}`; License: {RIME_CANTONESE_LICENSE}")

    # Literature: public-domain books, old forms changed to today's.
    unihan, _ = fetch(UNIHAN_URL, cache, "unihan/Unihan-16.0.0.zip", UNIHAN_SHA)
    everyday_counts = {}
    for p, _ in everyday:
        for ch in p:
            everyday_counts[ch] = everyday_counts.get(ch, 0) + 1
    variants = variant_map(unihan, os.path.join(root, "data", "sources", "table.txt"), everyday_counts)
    convert = str.maketrans(variants)
    literature, lit_heldout, lit_seen = [], [], set()
    for book, title in {**LIT_HELDOUT, **LIT_BOOKS}.items():  # Held-out first: shared text stays out.
        data, _ = fetch(GUTENBERG_URL.format(book), cache, f"gutenberg/txt/{book}.txt")
        target = lit_heldout if book in LIT_HELDOUT else literature
        for p in phrases(gutenberg_body(data).translate(convert)):
            if len(p) >= 2 and p not in lit_seen:  # Some books are also split into parts.
                lit_seen.add(p)
                target.append(p)
        notes.append(f"- Project Gutenberg #{book} {title}{' (held out)' if book in LIT_HELDOUT else ''} - "
                     f"{GUTENBERG_URL.format(book)}")
        notes.append(f"  - SHA-256: `{sha256(data)}`; License: {GUTENBERG_LICENSE}")
    notes.append(f"- Unihan variant data, for old character forms - {UNIHAN_URL}")
    notes.append(f"  - SHA-256: `{UNIHAN_SHA}`; License: {UNIHAN_LICENSE}")

    if not args.no_laws:
        data, _ = fetch(TW_LAW_URL, cache, "tw/ChLaw.json.zip")
        for text in tw_law_text(data):
            laws.extend(phrases(text))
        notes.append(f"- Taiwan 全國法規資料庫 中文法規 - {TW_LAW_URL}")
        notes.append(f"  - SHA-256: `{sha256(data)}`; License: {TW_LAW_LICENSE}")
        for url in HK_LEG_URLS:
            data, _ = fetch(url, cache, "hk/" + url.rsplit("/", 1)[1])
            for text in hk_leg_text(data):
                laws.extend(phrases(text))
            notes.append(f"- Hong Kong e-Legislation - {url}")
            notes.append(f"  - SHA-256: `{sha256(data)}`; License: {HK_LEG_LICENSE}")

    # Held-out test text: a fixed share of the everyday phrases, chosen by hash
    # so the split never changes between runs.
    heldout, seen = [], set()
    for p, region in everyday:
        if len(p) >= 2 and int(hashlib.md5(p.encode("utf-8")).hexdigest(), 16) % 100 < HELDOUT_PERCENT:
            if p not in seen:
                heldout.append((p, region))
                seen.add(p)
    held = set(seen)
    train = [p for p, _ in everyday if p not in held]
    # Laws repeat boilerplate a lot; keep each law phrase once.
    law_lines, law_seen = [], set()
    for p in laws:
        if p not in held and p not in law_seen:
            law_seen.add(p)
            law_lines.append(p)

    with open(os.path.join(args.out, "train.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(train) + "\n")
    with open(os.path.join(args.out, "laws.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(law_lines) + "\n")
    literature = [p for p in literature if p not in held]
    with open(os.path.join(args.out, "literature.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(literature) + "\n")
    with open(os.path.join(args.out, "heldout-lit.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lit_heldout) + "\n")
    for name, keep in (("heldout.txt", ("tw", "hk")), ("heldout-tw.txt", ("tw",)), ("heldout-hk.txt", ("hk",))):
        with open(os.path.join(args.out, name), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(p for p, region in heldout if region in keep) + "\n")
    notes += ["", "Attribution: Mozilla Common Voice contributors; Ministry of Justice, Taiwan (R.O.C.);",
              "Department of Justice, the Government of the Hong Kong Special Administrative Region (DATA.GOV.HK);",
              "K. K. Luke and May L.Y. Wong (HKCanCor); Tatoeba contributors; CanCLID (rime-cantonese);",
              "Project Gutenberg volunteers; Unicode, Inc. (Unihan).", ""]
    with open(os.path.join(args.out, "SOURCES.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(notes) + "\n")

    print(f"train: {len(train)} phrases, {sum(len(p) for p in train)} characters")
    print(f"laws: {len(law_lines)} phrases, {sum(len(p) for p in law_lines)} characters")
    print(f"literature: {len(literature)} phrases, {sum(len(p) for p in literature)} characters "
          f"({len(variants)} old forms mapped); heldout-lit: {len(lit_heldout)} phrases")
    for region in ("tw", "hk"):
        part = [p for p, r in heldout if r == region]
        print(f"heldout-{region}: {len(part)} phrases, {sum(len(p) for p in part)} characters")


if __name__ == "__main__":
    sys.exit(main())
