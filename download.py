#!/usr/bin/env python3
"""
Steam2 Dump Depot Indexer
--------------------------
A small GUI tool for browsing a steam2.download-style content dump.

It fetches the directory index pages for the dats/ and blobs/ folders,
parses filenames in the documented format:

    [depot_id]_[version]_[crc]_[hash]

...groups everything by depot_id, tracks all known versions/CRCs per
depot (which is what you need for the extractor, since content is
stored as deltas, and full resets are identified by CRC), optionally
resolves depot IDs to app/game names via the SteamDB API, and can
export everything to export.json.

Only uses the Python standard library - no pip installs required.

Usage:
    python3 steam2_depot_tool.py
"""

import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.request
import urllib.error
from html.parser import HTMLParser
from dataclasses import dataclass, field, asdict
from typing import Dict, List, Optional

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

DEFAULT_BASE_URL = "https://de.steam2.download/"
USER_AGENT = "steam2-depot-tool/1.0 (personal archival use)"

# Filename pattern: depotid_version_crc_hash  (hash may contain hex/alnum)
FILENAME_RE = re.compile(
    r"^(?P<depot_id>\d+)_(?P<version>\d+)_(?P<crc>[0-9a-fA-F]+)_(?P<hash>[0-9a-fA-F]+)"
    r"(?:\.(?P<ext>[A-Za-z0-9]+))?$"
)

REQUEST_TIMEOUT = 20


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass
class FileEntry:
    filename: str
    kind: str  # "dat" or "blob"
    version: str
    crc: str
    file_hash: str
    size: Optional[int] = None
    date: Optional[str] = None


@dataclass
class DepotInfo:
    depot_id: str
    app_id: Optional[str] = None
    app_name: Optional[str] = None
    depot_name: Optional[str] = None
    versions: List[str] = field(default_factory=list)
    crcs: List[str] = field(default_factory=list)
    manifest_ids: List[str] = field(default_factory=list)  # populated if resolvable
    dat_files: List[FileEntry] = field(default_factory=list)
    blob_files: List[FileEntry] = field(default_factory=list)
    likely_reset: bool = False  # true if duplicate versions/crcs seen -> full reset

    def to_export_dict(self):
        d = asdict(self)
        return d


# ---------------------------------------------------------------------------
# HTML index parsing (Apache/nginx style autoindex)
# ---------------------------------------------------------------------------

class DirIndexParser(HTMLParser):
    """Parses a simple Apache/nginx-style autoindex HTML page for <a href> links."""

    def __init__(self):
        super().__init__()
        self.links: List[str] = []

    def handle_starttag(self, tag, attrs):
        if tag.lower() == "a":
            for name, value in attrs:
                if name.lower() == "href" and value:
                    self.links.append(value)


def fetch_url(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT) as resp:
        charset = resp.headers.get_content_charset() or "utf-8"
        return resp.read().decode(charset, errors="replace")


def parse_index_page(html: str) -> List[str]:
    parser = DirIndexParser()
    parser.feed(html)
    # filter out parent-dir links, query-string sort links, etc.
    out = []
    for href in parser.links:
        if href.startswith("?"):
            continue
        if href in ("../", "/"):
            continue
        out.append(href)
    return out


# ---------------------------------------------------------------------------
# Core indexing logic
# ---------------------------------------------------------------------------

class DumpIndexer:
    def __init__(self, base_url: str, log_fn=print, stop_flag=None):
        self.base_url = base_url.rstrip("/") + "/"
        self.log = log_fn
        self.stop_flag = stop_flag or (lambda: False)
        self.depots: Dict[str, DepotInfo] = {}

    def _get_depot(self, depot_id: str) -> DepotInfo:
        if depot_id not in self.depots:
            self.depots[depot_id] = DepotInfo(depot_id=depot_id)
        return self.depots[depot_id]

    def _process_listing(self, kind: str, url: str):
        """kind is 'dat' or 'blob'. Fetches url (a directory index) and
        parses every filename link matching FILENAME_RE."""
        self.log(f"Fetching listing: {url}")
        try:
            html = fetch_url(url)
        except (urllib.error.URLError, TimeoutError) as e:
            self.log(f"  ! failed to fetch {url}: {e}")
            return

        links = parse_index_page(html)
        self.log(f"  found {len(links)} links")

        count = 0
        for href in links:
            if self.stop_flag():
                return
            name = href.rstrip("/").split("/")[-1]
            m = FILENAME_RE.match(name)
            if not m:
                continue
            depot_id = m.group("depot_id")
            version = m.group("version")
            crc = m.group("crc")
            file_hash = m.group("hash")

            entry = FileEntry(
                filename=name,
                kind=kind,
                version=version,
                crc=crc,
                file_hash=file_hash,
            )

            depot = self._get_depot(depot_id)
            if kind == "dat":
                depot.dat_files.append(entry)
            else:
                depot.blob_files.append(entry)

            if version not in depot.versions:
                depot.versions.append(version)
            if crc not in depot.crcs:
                depot.crcs.append(crc)

            count += 1

        self.log(f"  parsed {count} matching files from {url}")

    def index_from_bulk_text(self, kind: str, text: str):
        """Parse a bulk listing text file (e.g. dats_dates.txt / blobs.sha256
        style content, or any pasted list of filenames), one filename-ish
        token per line."""
        lines = text.splitlines()
        count = 0
        for line in lines:
            if self.stop_flag():
                return
            line = line.strip()
            if not line:
                continue
            # sha256 files look like: "<hash>  <filename>"
            # dates files might look like: "<filename>  <date>"
            # try to find a token in the line matching our filename pattern
            tokens = re.split(r"\s+", line)
            matched_name = None
            for tok in tokens:
                tok_clean = tok.strip().split("/")[-1]
                if FILENAME_RE.match(tok_clean):
                    matched_name = tok_clean
                    break
            if not matched_name:
                continue

            m = FILENAME_RE.match(matched_name)
            depot_id = m.group("depot_id")
            version = m.group("version")
            crc = m.group("crc")
            file_hash = m.group("hash")

            # try to find a date-like token too
            date_val = None
            for tok in tokens:
                if re.match(r"\d{4}-\d{2}-\d{2}", tok):
                    date_val = tok
                    break

            entry = FileEntry(
                filename=matched_name,
                kind=kind,
                version=version,
                crc=crc,
                file_hash=file_hash,
                date=date_val,
            )

            depot = self._get_depot(depot_id)
            if kind == "dat":
                depot.dat_files.append(entry)
            else:
                depot.blob_files.append(entry)

            if version not in depot.versions:
                depot.versions.append(version)
            if crc not in depot.crcs:
                depot.crcs.append(crc)

            count += 1

        self.log(f"Parsed {count} entries from pasted/bulk text ({kind})")

    def run_directory_crawl(self, subfolders=("dats/", "blobs/")):
        """Attempts to crawl the base_url's dats/ and blobs/ index pages.
        NOTE: many dumps disable directory listing for huge folders, or
        paginate; if this fails, use the bulk text import instead
        (dats_dates.txt / blobs_dates.txt / *.sha256 files work great for
        this and are much lighter to fetch than an HTML index of millions
        of files).
        """
        for sub in subfolders:
            if self.stop_flag():
                return
            kind = "dat" if "dat" in sub else "blob"
            url = self.base_url + sub
            self._process_listing(kind, url)

    def finalize(self):
        """Post-process: detect likely resets (depots with duplicate
        version numbers appearing against different CRCs, which per the
        dump's own readme indicates Valve did a full reset at some point).

        IMPORTANT: dat files and blob files for the SAME version naturally
        have DIFFERENT crcs (they're different files describing the same
        version). So resets must be detected separately within each kind
        (dat-vs-dat, blob-vs-blob) - comparing across kinds was a bug that
        flagged nearly every depot as "reset" incorrectly.
        """
        for depot in self.depots.values():
            depot.versions.sort(key=lambda v: (len(v), v))

            reset = False
            for file_list in (depot.dat_files, depot.blob_files):
                seen_versions = {}
                for f in file_list:
                    if f.version in seen_versions and seen_versions[f.version] != f.crc:
                        reset = True
                    seen_versions[f.version] = f.crc

            depot.likely_reset = reset


# ---------------------------------------------------------------------------
# Downloader
# ---------------------------------------------------------------------------

def sort_versions(versions: List[str]) -> List[str]:
    """Sort version strings numerically where possible."""
    def key(v):
        try:
            return (0, int(v))
        except ValueError:
            return (1, v)
    return sorted(versions, key=key)


def plan_download(depot: DepotInfo, target_version: Optional[str] = None) -> Dict[str, List[FileEntry]]:
    """Given a depot, figure out which dat/blob files are needed.

    Per the dump's own readme: content is delta-encoded, so to extract a
    given version you generally need every dat/blob up to and including
    that version. If the depot had a full reset (likely_reset), you only
    need the files from the most recent reset point onward, identified by
    matching CRC - the extractor's --blobcrc flag picks that starting
    point explicitly.

    This function returns the conservative, always-correct set: every
    file up to and including target_version (or all files, if
    target_version is None / "latest"). It does NOT try to be clever
    about reset points, since getting that wrong would produce a broken
    extraction - the user can always choose to only grab a subset
    manually if they know the reset boundary.
    """
    versions_sorted = sort_versions(depot.versions)
    if target_version is None or target_version == "latest":
        cutoff_versions = set(versions_sorted)
    else:
        try:
            idx = versions_sorted.index(target_version)
        except ValueError:
            idx = len(versions_sorted) - 1
        cutoff_versions = set(versions_sorted[: idx + 1])

    dat_needed = [f for f in depot.dat_files if f.version in cutoff_versions]
    blob_needed = [f for f in depot.blob_files if f.version in cutoff_versions]

    return {"dat": dat_needed, "blob": blob_needed}


class Downloader:
    """Handles downloading planned files to local dats/ and blobs/
    directories under a chosen destination folder, then optionally
    invoking the extractor binary from the dump."""

    def __init__(self, base_url: str, dest_dir: str, log_fn=print,
                 progress_fn=None, stop_flag=None):
        self.base_url = base_url.rstrip("/") + "/"
        self.dest_dir = dest_dir
        self.log = log_fn
        self.progress_fn = progress_fn or (lambda done, total, label: None)
        self.stop_flag = stop_flag or (lambda: False)

        self.dats_dir = os.path.join(dest_dir, "dats")
        self.blobs_dir = os.path.join(dest_dir, "blobs")
        os.makedirs(self.dats_dir, exist_ok=True)
        os.makedirs(self.blobs_dir, exist_ok=True)

    def _download_one(self, kind: str, filename: str) -> bool:
        sub = "dats" if kind == "dat" else "blobs"
        url = self.base_url + sub + "/" + filename
        dest_path = os.path.join(self.dats_dir if kind == "dat" else self.blobs_dir, filename)

        if os.path.exists(dest_path) and os.path.getsize(dest_path) > 0:
            self.log(f"  already have {filename}, skipping")
            return True

        tmp_path = dest_path + ".part"
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT) as resp, \
                    open(tmp_path, "wb") as out:
                chunk_size = 1024 * 256
                while True:
                    if self.stop_flag():
                        raise KeyboardInterrupt("stopped by user")
                    chunk = resp.read(chunk_size)
                    if not chunk:
                        break
                    out.write(chunk)
            os.replace(tmp_path, dest_path)
            return True
        except KeyboardInterrupt:
            self.log(f"  stopped while downloading {filename}")
            return False
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            self.log(f"  ! failed to download {filename}: {e}")
            if os.path.exists(tmp_path):
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass
            return False

    def download_plan(self, plan: Dict[str, List[FileEntry]]) -> bool:
        all_files = [("dat", f.filename) for f in plan["dat"]] + \
                    [("blob", f.filename) for f in plan["blob"]]
        total = len(all_files)
        self.log(f"Downloading {total} files ({len(plan['dat'])} dats, {len(plan['blob'])} blobs)...")

        ok_count = 0
        for i, (kind, filename) in enumerate(all_files, 1):
            if self.stop_flag():
                self.log("Download stopped by user.")
                return False
            self.progress_fn(i - 1, total, filename)
            success = self._download_one(kind, filename)
            if success:
                ok_count += 1
            self.progress_fn(i, total, filename)

        self.log(f"Downloaded {ok_count}/{total} files successfully.")
        return ok_count == total


def find_extractor_binary(search_dirs: List[str]) -> Optional[str]:
    """Look for the win64 extractor binary (extract / extract.exe) in the
    given directories (e.g. the extracted extractor/ folder from the dump)."""
    candidates = ["extract.exe", "extract"]
    for d in search_dirs:
        if not d or not os.path.isdir(d):
            continue
        for root, _, files in os.walk(d):
            for fname in files:
                if fname.lower() in candidates:
                    return os.path.join(root, fname)
    return None


def run_extractor(extractor_path: str, blobs_dir: str, dats_dir: str,
                   depot_id: str, version: str, blobcrc: Optional[str],
                   log_fn=print) -> bool:
    """Invokes the extractor binary per the dump's documented usage:
        extract <blobs_dir> <dats_dir> <depot_id> <version> [--blobcrc <crc>]

    On non-Windows systems this will only work if the binary is run via
    wine, per the dump's own readme (win64 build provided, Linux users
    must build from the included source themselves). This function will
    attempt to run it directly first, then fall back to trying via
    `wine` if direct execution fails and wine is available.
    """
    cmd = [extractor_path, blobs_dir, dats_dir, depot_id, version]
    if blobcrc:
        cmd += ["--blobcrc", blobcrc]

    def _run(command):
        log_fn(f"Running: {' '.join(command)}")
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=None
        )
        if result.stdout:
            log_fn(result.stdout)
        if result.stderr:
            log_fn(result.stderr)
        return result.returncode == 0

    try:
        if _run(cmd):
            return True
    except (OSError, subprocess.SubprocessError) as e:
        log_fn(f"Direct execution failed: {e}")

    if sys.platform != "win32" and extractor_path.lower().endswith(".exe"):
        log_fn("Attempting to run via wine...")
        try:
            if _run(["wine", extractor_path, blobs_dir, dats_dir, depot_id, version]
                    + (["--blobcrc", blobcrc] if blobcrc else [])):
                return True
        except (OSError, subprocess.SubprocessError) as e:
            log_fn(f"wine execution also failed: {e}")
            log_fn("You may need to build the extractor from source for your platform "
                   "(source is included in the dump's extractor/ folder).")

    return False


# ---------------------------------------------------------------------------
# SteamDB resolution (best-effort, optional)
# ---------------------------------------------------------------------------

def resolve_depot_via_steamdb_api(depot_id: str) -> Optional[dict]:
    """Best-effort lookup using SteamDB's unofficial/public JSON where
    available. This is NOT guaranteed stable — SteamDB has no official
    public API for this. If it fails, we just leave the name blank and
    you can fill it in manually by visiting steamdb.info/depot/<id>/
    """
    url = f"https://steamdb.info/api/GetAppSubmittableApps/?tag=depot&depotid={depot_id}"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT) as resp:
            data = json.loads(resp.read().decode("utf-8", errors="replace"))
            return data
    except Exception:
        return None


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Steam2 Dump Depot Indexer")
        self.geometry("980x680")

        self.indexer: Optional[DumpIndexer] = None
        self._stop_requested = False
        self._worker_thread: Optional[threading.Thread] = None

        self._build_ui()

    # ---------------------------------------------------------------
    def _build_ui(self):
        top = ttk.Frame(self, padding=8)
        top.pack(fill="x")

        ttk.Label(top, text="Base URL:").grid(row=0, column=0, sticky="w")
        self.base_url_var = tk.StringVar(value=DEFAULT_BASE_URL)
        ttk.Entry(top, textvariable=self.base_url_var, width=50).grid(
            row=0, column=1, sticky="we", padx=4
        )

        ttk.Button(top, text="Crawl dats/ + blobs/ index", command=self.on_crawl).grid(
            row=0, column=2, padx=4
        )
        ttk.Button(top, text="Import bulk text file(s)...", command=self.on_import_bulk).grid(
            row=0, column=3, padx=4
        )
        ttk.Button(top, text="Paste listing text...", command=self.on_paste_listing).grid(
            row=0, column=4, padx=4
        )

        top.columnconfigure(1, weight=1)

        mid = ttk.Frame(self, padding=(8, 0))
        mid.pack(fill="both", expand=True)

        # Left: depot table
        left = ttk.Frame(mid)
        left.pack(side="left", fill="both", expand=True)

        ttk.Label(left, text="Depots found:").pack(anchor="w")

        columns = ("depot_id", "versions", "crcs", "reset", "dat_files", "blob_files", "app_name")
        self.tree = ttk.Treeview(left, columns=columns, show="headings", height=20)
        headings = {
            "depot_id": "Depot ID",
            "versions": "# Versions",
            "crcs": "# CRCs",
            "reset": "Reset?",
            "dat_files": "# DATs",
            "blob_files": "# Blobs",
            "app_name": "App/Game (if resolved)",
        }
        widths = {
            "depot_id": 80,
            "versions": 80,
            "crcs": 70,
            "reset": 60,
            "dat_files": 70,
            "blob_files": 70,
            "app_name": 260,
        }
        for c in columns:
            self.tree.heading(c, text=headings[c])
            self.tree.column(c, width=widths[c], anchor="center")
        self.tree.column("app_name", anchor="w")
        self.tree.pack(fill="both", expand=True, side="left")

        scroll = ttk.Scrollbar(left, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscroll=scroll.set)
        scroll.pack(side="right", fill="y")

        self.tree.bind("<<TreeviewSelect>>", self.on_select_depot)

        # Right: detail + actions
        right = ttk.Frame(mid, padding=(8, 0))
        right.pack(side="left", fill="both", expand=False)

        ttk.Label(right, text="Depot detail:").pack(anchor="w")
        self.detail_text = scrolledtext.ScrolledText(right, width=48, height=20, wrap="word")
        self.detail_text.pack(fill="both", expand=True)

        ttk.Label(right, text="Manual SteamDB name (optional):").pack(anchor="w", pady=(6, 0))
        self.manual_name_var = tk.StringVar()
        name_row = ttk.Frame(right)
        name_row.pack(fill="x")
        ttk.Entry(name_row, textvariable=self.manual_name_var).pack(side="left", fill="x", expand=True)
        ttk.Button(name_row, text="Save name", command=self.on_save_manual_name).pack(side="left", padx=4)

        ttk.Button(
            right, text="Open steamdb.info page for selected depot",
            command=self.on_open_steamdb
        ).pack(fill="x", pady=4)

        ttk.Button(
            right, text="Try auto-resolve name via SteamDB (best-effort)",
            command=self.on_resolve_selected
        ).pack(fill="x", pady=4)

        ttk.Separator(right, orient="horizontal").pack(fill="x", pady=8)

        ttk.Label(right, text="Download & Extract:", font=("", 10, "bold")).pack(anchor="w")

        target_row = ttk.Frame(right)
        target_row.pack(fill="x", pady=(4, 0))
        ttk.Label(target_row, text="Target version:").pack(side="left")
        self.target_version_var = tk.StringVar(value="latest")
        self.target_version_combo = ttk.Combobox(
            target_row, textvariable=self.target_version_var, width=14, state="readonly"
        )
        self.target_version_combo.pack(side="left", padx=4)

        ttk.Button(
            right, text="Choose destination folder...", command=self.on_choose_dest
        ).pack(fill="x", pady=(6, 0))
        self.dest_dir_var = tk.StringVar(value="")
        ttk.Label(right, textvariable=self.dest_dir_var, wraplength=380, foreground="#555").pack(
            anchor="w", pady=(2, 4)
        )

        ttk.Button(
            right, text="Download depot files", command=self.on_download_depot
        ).pack(fill="x", pady=2)

        self.download_progress = ttk.Progressbar(right, mode="determinate")
        self.download_progress.pack(fill="x", pady=4)
        self.download_label_var = tk.StringVar(value="")
        ttk.Label(right, textvariable=self.download_label_var, foreground="#555").pack(anchor="w")

        ttk.Separator(right, orient="horizontal").pack(fill="x", pady=8)

        ttk.Label(right, text="Extractor binary (extract / extract.exe):").pack(anchor="w")
        extractor_row = ttk.Frame(right)
        extractor_row.pack(fill="x")
        self.extractor_path_var = tk.StringVar(value="")
        ttk.Entry(extractor_row, textvariable=self.extractor_path_var).pack(
            side="left", fill="x", expand=True
        )
        ttk.Button(extractor_row, text="Browse...", command=self.on_choose_extractor).pack(
            side="left", padx=4
        )
        ttk.Button(
            extractor_row, text="Auto-find", command=self.on_autofind_extractor
        ).pack(side="left")

        ttk.Button(
            right, text="Run extractor on downloaded files", command=self.on_run_extractor
        ).pack(fill="x", pady=(4, 0))

        bottom = ttk.Frame(self, padding=8)
        bottom.pack(fill="x")

        ttk.Button(bottom, text="Export to export.json", command=self.on_export).pack(
            side="left", padx=4
        )
        ttk.Button(bottom, text="Stop current operation", command=self.on_stop).pack(
            side="left", padx=4
        )
        self.status_var = tk.StringVar(value="Ready.")
        ttk.Label(bottom, textvariable=self.status_var).pack(side="left", padx=12)

        self.log_text = scrolledtext.ScrolledText(self, height=8, wrap="word", state="disabled")
        self.log_text.pack(fill="x", padx=8, pady=(0, 8))

    # ---------------------------------------------------------------
    def log(self, msg: str):
        def _do():
            self.log_text.configure(state="normal")
            self.log_text.insert("end", msg + "\n")
            self.log_text.see("end")
            self.log_text.configure(state="disabled")
        self.after(0, _do)

    def set_status(self, msg: str):
        self.after(0, lambda: self.status_var.set(msg))

    def _ensure_indexer(self):
        if self.indexer is None:
            self.indexer = DumpIndexer(
                self.base_url_var.get().strip(),
                log_fn=self.log,
                stop_flag=lambda: self._stop_requested,
            )
        return self.indexer

    # ---------------------------------------------------------------
    def on_crawl(self):
        if self._worker_thread and self._worker_thread.is_alive():
            messagebox.showinfo("Busy", "An operation is already running.")
            return
        self._stop_requested = False
        indexer = self._ensure_indexer()
        indexer.base_url = self.base_url_var.get().strip().rstrip("/") + "/"

        def work():
            self.set_status("Crawling dats/ and blobs/ index pages...")
            indexer.run_directory_crawl()
            indexer.finalize()
            self.set_status(f"Done. {len(indexer.depots)} depots found so far.")
            self.after(0, self.refresh_tree)

        self._worker_thread = threading.Thread(target=work, daemon=True)
        self._worker_thread.start()

    def on_import_bulk(self):
        paths = filedialog.askopenfilenames(
            title="Select bulk listing file(s) (e.g. dats_dates.txt, blobs.sha256)",
            filetypes=[("Text files", "*.txt *.sha256"), ("All files", "*.*")],
        )
        if not paths:
            return
        indexer = self._ensure_indexer()

        def work():
            for p in paths:
                kind = "dat" if "dat" in p.lower() else "blob"
                self.set_status(f"Importing {p} as '{kind}'...")
                try:
                    with open(p, "r", encoding="utf-8", errors="replace") as f:
                        text = f.read()
                except Exception as e:
                    self.log(f"! failed to read {p}: {e}")
                    continue
                indexer.index_from_bulk_text(kind, text)
            indexer.finalize()
            self.set_status(f"Done. {len(indexer.depots)} depots found so far.")
            self.after(0, self.refresh_tree)

        self._worker_thread = threading.Thread(target=work, daemon=True)
        self._worker_thread.start()

    def on_paste_listing(self):
        win = tk.Toplevel(self)
        win.title("Paste listing text")
        win.geometry("600x500")

        ttk.Label(win, text="Kind:").pack(anchor="w", padx=8, pady=(8, 0))
        kind_var = tk.StringVar(value="dat")
        kind_row = ttk.Frame(win)
        kind_row.pack(anchor="w", padx=8)
        ttk.Radiobutton(kind_row, text="dats", variable=kind_var, value="dat").pack(side="left")
        ttk.Radiobutton(kind_row, text="blobs", variable=kind_var, value="blob").pack(side="left")

        ttk.Label(win, text="Paste directory listing / filename list below:").pack(
            anchor="w", padx=8, pady=(8, 0)
        )
        text_widget = scrolledtext.ScrolledText(win, wrap="word")
        text_widget.pack(fill="both", expand=True, padx=8, pady=4)

        def do_import():
            content = text_widget.get("1.0", "end")
            indexer = self._ensure_indexer()

            def work():
                indexer.index_from_bulk_text(kind_var.get(), content)
                indexer.finalize()
                self.set_status(f"Done. {len(indexer.depots)} depots found so far.")
                self.after(0, self.refresh_tree)

            threading.Thread(target=work, daemon=True).start()
            win.destroy()

        ttk.Button(win, text="Import", command=do_import).pack(pady=8)

    def on_stop(self):
        self._stop_requested = True
        self.set_status("Stop requested (will halt after current fetch).")

    # ---------------------------------------------------------------
    def refresh_tree(self):
        self.tree.delete(*self.tree.get_children())
        if not self.indexer:
            return
        for depot_id, depot in sorted(self.indexer.depots.items(), key=lambda kv: int(kv[0])):
            self.tree.insert(
                "",
                "end",
                iid=depot_id,
                values=(
                    depot.depot_id,
                    len(depot.versions),
                    len(depot.crcs),
                    "yes" if depot.likely_reset else "no",
                    len(depot.dat_files),
                    len(depot.blob_files),
                    depot.app_name or depot.depot_name or "",
                ),
            )

    def _selected_depot(self) -> Optional[DepotInfo]:
        sel = self.tree.selection()
        if not sel or not self.indexer:
            return None
        return self.indexer.depots.get(sel[0])

    def on_select_depot(self, event=None):
        depot = self._selected_depot()
        self.detail_text.delete("1.0", "end")
        if not depot:
            return

        lines = []
        lines.append(f"Depot ID: {depot.depot_id}")
        lines.append(f"App name: {depot.app_name or '(unresolved)'}")
        lines.append(f"Depot name: {depot.depot_name or '(unresolved)'}")
        lines.append(f"Likely full reset at some point: {depot.likely_reset}")
        lines.append("")
        lines.append(f"Versions ({len(depot.versions)}): {', '.join(depot.versions[:50])}")
        if len(depot.versions) > 50:
            lines.append(f"  ...and {len(depot.versions) - 50} more")
        lines.append("")
        lines.append(f"CRCs ({len(depot.crcs)}): {', '.join(depot.crcs[:50])}")
        if len(depot.crcs) > 50:
            lines.append(f"  ...and {len(depot.crcs) - 50} more")
        lines.append("")
        lines.append(f"DAT files: {len(depot.dat_files)}")
        for f in depot.dat_files[:20]:
            lines.append(f"  {f.filename}")
        if len(depot.dat_files) > 20:
            lines.append(f"  ...and {len(depot.dat_files) - 20} more")
        lines.append("")
        lines.append(f"Blob files: {len(depot.blob_files)}")
        for f in depot.blob_files[:20]:
            lines.append(f"  {f.filename}")
        if len(depot.blob_files) > 20:
            lines.append(f"  ...and {len(depot.blob_files) - 20} more")

        lines.append("")
        lines.append("Suggested extractor command:")
        if depot.likely_reset and depot.crcs:
            lines.append(
                f"  extract <blobs_dir> <dats_dir> {depot.depot_id} <version> --blobcrc {depot.crcs[-1]}"
            )
        else:
            lines.append(f"  extract <blobs_dir> <dats_dir> {depot.depot_id} <version>")
        lines.append("  (download ALL prior version dat/blob files too - content is delta-encoded)")

        self.detail_text.insert("1.0", "\n".join(lines))
        self.manual_name_var.set(depot.app_name or depot.depot_name or "")

        versions_sorted = sort_versions(depot.versions)
        self.target_version_combo["values"] = ["latest"] + versions_sorted
        self.target_version_var.set("latest")

    def on_save_manual_name(self):
        depot = self._selected_depot()
        if not depot:
            return
        name = self.manual_name_var.get().strip()
        depot.app_name = name or None
        self.refresh_tree()
        # reselect
        self.tree.selection_set(depot.depot_id)

    def on_open_steamdb(self):
        depot = self._selected_depot()
        if not depot:
            return
        import webbrowser
        webbrowser.open(f"https://steamdb.info/depot/{depot.depot_id}/")

    def on_resolve_selected(self):
        depot = self._selected_depot()
        if not depot:
            return

        def work():
            self.set_status(f"Attempting best-effort SteamDB resolution for depot {depot.depot_id}...")
            data = resolve_depot_via_steamdb_api(depot.depot_id)
            if data:
                self.log(f"SteamDB API response for {depot.depot_id}: {json.dumps(data)[:500]}")
                self.set_status("Got a response - check log. This API is unofficial and may not contain a name.")
            else:
                self.set_status(
                    "Auto-resolve failed (expected - SteamDB has no stable public API for this). "
                    "Please open the depot page manually and enter the name."
                )

        threading.Thread(target=work, daemon=True).start()

    # ---------------------------------------------------------------
    def on_choose_dest(self):
        path = filedialog.askdirectory(title="Choose destination folder for downloaded files")
        if path:
            self.dest_dir_var.set(path)

    def on_download_depot(self):
        depot = self._selected_depot()
        if not depot:
            messagebox.showinfo("No depot selected", "Select a depot in the table first.")
            return
        dest = self.dest_dir_var.get().strip()
        if not dest:
            messagebox.showinfo("No destination", "Choose a destination folder first.")
            return
        if self._worker_thread and self._worker_thread.is_alive():
            messagebox.showinfo("Busy", "An operation is already running.")
            return

        target_version = self.target_version_var.get()
        plan = plan_download(depot, None if target_version == "latest" else target_version)
        total_files = len(plan["dat"]) + len(plan["blob"])
        if total_files == 0:
            messagebox.showinfo("Nothing to download", "No files matched the selected version.")
            return

        if not messagebox.askyesno(
            "Confirm download",
            f"This will download {total_files} files "
            f"({len(plan['dat'])} dats + {len(plan['blob'])} blobs) for depot "
            f"{depot.depot_id} up to version {target_version}.\n\n"
            f"Destination: {dest}\n\n"
            f"Note: content is delta-encoded, so all prior versions are included "
            f"automatically per the dump's readme. Continue?",
        ):
            return

        self._stop_requested = False
        base_url = self.base_url_var.get().strip()

        def progress(done, total, label):
            self.after(0, lambda: self._update_download_progress(done, total, label))

        def work():
            self.set_status(f"Downloading depot {depot.depot_id}...")
            downloader = Downloader(
                base_url, dest, log_fn=self.log,
                progress_fn=progress,
                stop_flag=lambda: self._stop_requested,
            )
            success = downloader.download_plan(plan)
            if success:
                self.set_status(f"Download complete for depot {depot.depot_id}.")
            else:
                self.set_status(f"Download incomplete/stopped for depot {depot.depot_id}.")

        self._worker_thread = threading.Thread(target=work, daemon=True)
        self._worker_thread.start()

    def _update_download_progress(self, done, total, label):
        self.download_progress["maximum"] = max(total, 1)
        self.download_progress["value"] = done
        self.download_label_var.set(f"{done}/{total}  {label}")

    def on_choose_extractor(self):
        path = filedialog.askopenfilename(
            title="Select extractor binary",
            filetypes=[("Executable", "*.exe"), ("All files", "*.*")],
        )
        if path:
            self.extractor_path_var.set(path)

    def on_autofind_extractor(self):
        dest = self.dest_dir_var.get().strip()
        search_dirs = [dest, os.getcwd()]
        found = find_extractor_binary(search_dirs)
        if found:
            self.extractor_path_var.set(found)
            self.set_status(f"Found extractor: {found}")
        else:
            messagebox.showinfo(
                "Not found",
                "Couldn't auto-find the extractor binary. Browse for it manually - "
                "it's included in the dump's extractor/ folder (extract.exe for win64).",
            )

    def on_run_extractor(self):
        depot = self._selected_depot()
        if not depot:
            messagebox.showinfo("No depot selected", "Select a depot in the table first.")
            return
        dest = self.dest_dir_var.get().strip()
        if not dest:
            messagebox.showinfo("No destination", "Choose a destination folder first (where files were downloaded).")
            return
        extractor_path = self.extractor_path_var.get().strip()
        if not extractor_path or not os.path.isfile(extractor_path):
            messagebox.showinfo("No extractor", "Select a valid extractor binary first.")
            return

        target_version = self.target_version_var.get()
        if target_version == "latest":
            versions_sorted = sort_versions(depot.versions)
            if not versions_sorted:
                messagebox.showinfo("No versions", "This depot has no known versions.")
                return
            target_version = versions_sorted[-1]

        blobcrc = None
        if depot.likely_reset and depot.crcs:
            if messagebox.askyesno(
                "Reset detected",
                f"Depot {depot.depot_id} appears to have had a full reset at some point.\n\n"
                f"Do you want to specify a --blobcrc to pick a specific reset point? "
                f"(Choose 'No' to let the extractor use default delta behavior, which "
                f"may be incorrect for reset depots.)",
            ):
                blobcrc = self.manual_name_var.get().strip() or None
                if not blobcrc:
                    # prompt via simple dialog using the crcs list
                    blobcrc = self._ask_choice("Choose CRC", "Select the blob CRC:", depot.crcs)

        dats_dir = os.path.join(dest, "dats")
        blobs_dir = os.path.join(dest, "blobs")

        def work():
            self.set_status(f"Running extractor for depot {depot.depot_id} v{target_version}...")
            ok = run_extractor(
                extractor_path, blobs_dir, dats_dir,
                depot.depot_id, target_version, blobcrc,
                log_fn=self.log,
            )
            if ok:
                self.set_status(f"Extraction complete for depot {depot.depot_id}.")
                self.after(0, lambda: messagebox.showinfo(
                    "Extraction complete",
                    f"Extractor finished for depot {depot.depot_id} version {target_version}.\n"
                    f"Check the destination folder for output.",
                ))
            else:
                self.set_status(f"Extraction failed for depot {depot.depot_id} - check log.")

        threading.Thread(target=work, daemon=True).start()

    def _ask_choice(self, title, prompt, choices) -> Optional[str]:
        win = tk.Toplevel(self)
        win.title(title)
        win.geometry("300x120")
        ttk.Label(win, text=prompt).pack(pady=8)
        var = tk.StringVar(value=choices[0] if choices else "")
        combo = ttk.Combobox(win, textvariable=var, values=choices, state="readonly")
        combo.pack(pady=4, padx=8, fill="x")

        result = {"value": None}

        def on_ok():
            result["value"] = var.get()
            win.destroy()

        ttk.Button(win, text="OK", command=on_ok).pack(pady=8)
        win.grab_set()
        self.wait_window(win)
        return result["value"]

    # ---------------------------------------------------------------
    def on_export(self):
        if not self.indexer or not self.indexer.depots:
            messagebox.showinfo("Nothing to export", "No depots indexed yet.")
            return
        path = filedialog.asksaveasfilename(
            title="Save export.json",
            defaultextension=".json",
            initialfile="export.json",
            filetypes=[("JSON files", "*.json")],
        )
        if not path:
            return

        export_data = {
            "base_url": self.indexer.base_url,
            "generated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "depot_count": len(self.indexer.depots),
            "depots": {
                depot_id: depot.to_export_dict()
                for depot_id, depot in self.indexer.depots.items()
            },
        }

        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(export_data, f, indent=2)
        except Exception as e:
            messagebox.showerror("Export failed", str(e))
            return

        messagebox.showinfo("Exported", f"Saved {len(self.indexer.depots)} depots to:\n{path}")


def main():
    app = App()
    app.mainloop()


if __name__ == "__main__":
    main()
