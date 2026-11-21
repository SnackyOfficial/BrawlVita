"""BrawlVita installer (Windows GUI). Turns YOUR OWN two v29 APKs into the files BrawlVita needs and puts them on your Vita.

What it does (nothing from the APKs is ever executed):
  1. checks the offline v29 APK (libraries: SHA-256) and the original v29 APK
  2. copies the two libraries + the assets from the offline APK, and Colette's 3D model files from the original APK
  3. optionally downloads the game's high-resolution pictures that were never inside the APK (official asset server, SHA-1 checked)
  4. writes config.txt with the chosen profile (normal / modded / max)
  5. builds BrawlVita.vpk with your cover pictures (converted to the Vita's format) or the default black / white ones
  6. delivers everything to a folder, to the Vita over USB (VitaShell USB mode) or over FTP (VitaShell FTP server)

Build the .exe: installer/build_exe.bat (needs Python 3 with Pillow and PyInstaller).
"""
import ftplib
import hashlib
import io
import json
import os
import queue
import shutil
import string
import sys
import tempfile
import threading
import time
import urllib.request
import zipfile

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from PIL import Image, ImageDraw, ImageFont

VERSION = "0.1 beta"
OFFLINE_LIBS = {
    "libg.so": "a96de7c77ef6fdf850eab24fdcf35c3ab93d0b52135b15dc82c8e23669e0574d",
    "libfmod.so": "a7d6bf28b1f89a672197a081a4c80742813c4e3e33014a84859dac7192201c00",
}
ORIGINAL_LIBG = "5bb24854b40b1a5b"          # first 16 hex digits of the original v29.270 libg.so (a sanity check only)
ASSET_HOST = "http://game-assets.brawlstarsgame.com"
COVERS = {  # VPK path, size
    "icon": ("sce_sys/icon0.png", (128, 128)),
    "bg": ("sce_sys/livearea/contents/bg.png", (840, 500)),
    "startup": ("sce_sys/livearea/contents/startup.png", (280, 158)),
}
KUBRIDGE_URL = "https://github.com/TheOfficialFloW/kubridge/releases/latest/download/kubridge.skprx"


def resource(name):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)


# ---------------------------------------------------------------- images
def font(size):
    for name in ("arialbd.ttf", "Arial Bold.ttf", "DejaVuSans-Bold.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def default_cover(size):
    w, h = size
    img = Image.new("RGB", size, (0, 0, 0))
    d = ImageDraw.Draw(img)
    text = "BRAWLVITA"
    s = max(8, int(w / 6.2))
    while s > 8 and d.textlength(text, font=font(s)) > w * 0.86:
        s -= 1
    f = font(s)
    tw = d.textlength(text, font=f)
    bbox = d.textbbox((0, 0), text, font=f)
    d.text(((w - tw) / 2, (h - (bbox[3] - bbox[1])) / 2 - bbox[1]), text, font=f, fill=(255, 255, 255))
    return img


def vita_png(img, size):
    """Cover-crop to the size, then 8-bit indexed PNG (the Vita's installer refuses other PNG types)."""
    img = img.convert("RGB")
    w, h = size
    scale = max(w / img.width, h / img.height)
    img = img.resize((max(w, round(img.width * scale)), max(h, round(img.height * scale))), Image.LANCZOS)
    left, top = (img.width - w) // 2, (img.height - h) // 2
    img = img.crop((left, top, left + w, top + h))
    img = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    out = io.BytesIO()
    img.save(out, format="PNG", optimize=False)
    return out.getvalue()


# ---------------------------------------------------------------- work
class Job:
    def __init__(self, opts, log, progress):
        self.o = opts
        self.log = log
        self.progress = progress

    def run(self):
        o = self.o
        stage = tempfile.mkdtemp(prefix="brawlvita_") if o["mode"] != "folder" else os.path.join(o["folder"], "BrawlVita_install")
        data = os.path.join(stage, "ux0_data_BrawlVita")
        os.makedirs(os.path.join(data, "lib"), exist_ok=True)
        try:
            self.extract(data)
            if o["download"]:
                self.download(data)
            with open(os.path.join(data, "config.txt"), "w", newline="\n") as fh:
                fh.write("profile=%s\n" % o["profile"])
            vpk = os.path.join(stage, "BrawlVita.vpk")
            self.make_vpk(vpk)
            if o["mode"] == "folder":
                self.write_readme(stage)
                self.log("Done. Copy '%s' to the Vita as ux0:data/BrawlVita/ and install BrawlVita.vpk with VitaShell." % data)
            elif o["mode"] == "usb":
                self.to_usb(data, vpk)
            else:
                self.to_ftp(data, vpk)
            self.progress(100)
            return True
        finally:
            if o["mode"] != "folder":
                shutil.rmtree(stage, ignore_errors=True)

    def extract(self, data):
        o = self.o
        self.log("Checking the offline v29 APK ...")
        with zipfile.ZipFile(o["offline"]) as z:
            for name, want in OFFLINE_LIBS.items():
                try:
                    blob = z.read("lib/armeabi-v7a/" + name)
                except KeyError:
                    raise RuntimeError("The offline APK has no lib/armeabi-v7a/%s." % name)
                if hashlib.sha256(blob).hexdigest() != want:
                    raise RuntimeError("The offline APK's %s is not the build BrawlVita needs (wrong file?)." % name)
                with open(os.path.join(data, "lib", name), "wb") as fh:
                    fh.write(blob)
            self.log("Libraries OK. Copying the game assets (about 260 MB) ...")
            entries = [i for i in z.infolist() if i.filename.startswith("assets/") and not i.is_dir()]
            if o["no_music"]:
                entries = [i for i in entries if i.filename.split("/")[1] not in ("music", "sfx")]
            base = os.path.join(data, "assets") + os.sep
            for k, info in enumerate(entries):
                dest = os.path.abspath(os.path.join(data, info.filename))
                if not dest.startswith(base):
                    raise RuntimeError("unsafe entry " + info.filename)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with z.open(info) as src, open(dest, "wb") as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                if k % 200 == 0:
                    self.progress(5 + 35 * k / max(1, len(entries)))
        self.log("Checking the original v29 APK (Colette's model) ...")
        with zipfile.ZipFile(o["original"]) as z:
            try:
                h = hashlib.sha256(z.read("lib/armeabi-v7a/libg.so")).hexdigest()
                if not h.startswith(ORIGINAL_LIBG):
                    self.log("  note: this original APK's libg.so is not the 29.270 one; using its Colette files anyway")
            except KeyError:
                self.log("  note: no libg.so in the original APK (split APK?); using its Colette files anyway")
            n = 0
            for info in z.infolist():
                f = info.filename
                if f.startswith("assets/sc3d/colette") and not info.is_dir():
                    with z.open(info) as src, open(os.path.join(data, f), "wb") as dst:
                        shutil.copyfileobj(src, dst)
                    n += 1
            self.log("  %d Colette files taken from the original APK" % n)
        self.progress(45)

    def download(self, data):
        fp_path = os.path.join(data, "assets", "fingerprint.json")
        try:
            fp = json.load(open(fp_path, encoding="utf-8"))
        except Exception:
            self.log("No fingerprint.json in the assets: skipping the picture download.")
            return
        todo = [f for f in fp.get("files", []) if f.get("defer") and not f.get("ios")]
        self.log("Downloading %d high-resolution pictures / sounds from the game's asset server ..." % len(todo))
        ok = bad = 0
        for k, f in enumerate(todo):
            dest = os.path.join(data, "assets", f["file"].replace("/", os.sep))
            url = "%s/%s/%s" % (ASSET_HOST, fp["sha"], f["file"])
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "BrawlVita-installer"})
                blob = urllib.request.urlopen(req, timeout=60).read()
                if hashlib.sha1(blob).hexdigest() != f["sha"]:
                    raise ValueError("checksum")
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as fh:
                    fh.write(blob)
                ok += 1
            except Exception as e:
                bad += 1
                if bad <= 3:
                    self.log("  could not get %s (%s)" % (f["file"], e))
            self.progress(45 + 20 * (k + 1) / max(1, len(todo)))
        self.log("  %d downloaded, %d failed (failed ones only mean some skin previews stay blank)" % (ok, bad))

    def make_vpk(self, out):
        base = resource("BrawlVita.vpk")
        if not os.path.exists(base):
            raise RuntimeError("BrawlVita.vpk is missing next to the installer.")
        covers = {}
        for key, (path, size) in COVERS.items():
            src = self.o["covers"].get(key)
            img = Image.open(src) if src else default_cover(size)
            covers[path] = vita_png(img, size)
        with zipfile.ZipFile(base) as zin, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zout:
            for info in zin.infolist():
                blob = covers.get(info.filename)
                zout.writestr(info, blob if blob is not None else zin.read(info.filename))
        self.log("BrawlVita.vpk written (%s covers)." % ("your" if any(self.o["covers"].values()) else "default"))
        self.progress(70)

    def write_readme(self, stage):
        with open(os.path.join(stage, "HOW_TO_INSTALL.txt"), "w", newline="\r\n") as fh:
            fh.write("1. Copy the folder ux0_data_BrawlVita to your Vita as ux0:data/BrawlVita (rename it there).\n"
                     "2. Copy BrawlVita.vpk to the Vita and install it with VitaShell.\n"
                     "3. Make sure the kubridge plugin is installed (see the BrawlVita README).\n"
                     "4. Start Brawl Vita. The first start takes 1-2 minutes.\n")

    def files_of(self, data):
        out = []
        for base, _, names in os.walk(data):
            for n in names:
                p = os.path.join(base, n)
                out.append((p, os.path.relpath(p, data).replace(os.sep, "/")))
        return out

    def to_usb(self, data, vpk):
        root = self.o["usb"]
        if not os.path.isdir(os.path.join(root, "data")):
            raise RuntimeError("%s does not look like the Vita's memory card (no 'data' folder). Start VitaShell's USB mode first." % root)
        dest = os.path.join(root, "data", "BrawlVita")
        files = self.files_of(data)
        self.log("Copying %d files to %s ..." % (len(files), dest))
        for k, (p, rel) in enumerate(files):
            d = os.path.join(dest, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copyfile(p, d)
            if k % 100 == 0:
                self.progress(70 + 28 * k / max(1, len(files)))
        shutil.copyfile(vpk, os.path.join(root, "BrawlVita.vpk"))
        self.log("Done. On the Vita: leave USB mode, install ux0:BrawlVita.vpk with VitaShell, then start Brawl Vita.")

    def to_ftp(self, data, vpk):
        o = self.o
        self.log("Connecting to the Vita at %s:%s (VitaShell FTP) ..." % (o["ip"], o["port"]))
        ftp = ftplib.FTP()
        ftp.connect(o["ip"], int(o["port"]), timeout=30)
        ftp.login()
        made = set()

        def mkdirs(path):
            parts = path.strip("/").split("/")
            cur = ""
            for part in parts:
                cur += "/" + part
                if cur in made:
                    continue
                try:
                    ftp.mkd(cur)
                except ftplib.error_perm:
                    pass
                made.add(cur)

        files = self.files_of(data)
        self.log("Uploading %d files (this takes a while) ..." % len(files))
        for k, (p, rel) in enumerate(files):
            remote = "/ux0:/data/BrawlVita/" + rel
            mkdirs(os.path.dirname(remote))
            with open(p, "rb") as fh:
                ftp.storbinary("STOR " + remote, fh)
            if k % 50 == 0:
                self.progress(70 + 25 * k / max(1, len(files)))
        with open(vpk, "rb") as fh:
            ftp.storbinary("STOR /ux0:/BrawlVita.vpk", fh)
        if o["kubridge"]:
            self.install_kubridge(ftp)
        ftp.quit()
        self.log("Done. On the Vita: install ux0:BrawlVita.vpk with VitaShell, then start Brawl Vita.")

    def install_kubridge(self, ftp):
        """kubridge (by TheOfficialFloW) into ur0:tai, listed under *KERNEL of the active taiHEN config; the old config is backed up first."""
        cfg_path = None
        for cand in ("/ux0:/tai/config.txt", "/ur0:/tai/config.txt"):   # taiHEN uses ux0:tai/config.txt when it exists
            buf = io.BytesIO()
            try:
                ftp.retrbinary("RETR " + cand, buf.write)
                cfg_path, cfg = cand, buf.getvalue().decode("utf-8", "replace")
                break
            except ftplib.all_errors:
                continue
        if not cfg_path:
            self.log("kubridge: no taiHEN config.txt found - install kubridge by hand (README).")
            return
        if "kubridge.skprx" in cfg:
            self.log("kubridge: already in %s" % cfg_path)
            return
        self.log("kubridge: downloading the plugin from its GitHub release ...")
        blob = urllib.request.urlopen(urllib.request.Request(KUBRIDGE_URL, headers={"User-Agent": "BrawlVita-installer"}), timeout=60).read()
        ftp.storbinary("STOR /ur0:/tai/kubridge.skprx", io.BytesIO(blob))
        ftp.storbinary("STOR %s.brawlvita_backup_%d" % (cfg_path, int(time.time())), io.BytesIO(cfg.encode("utf-8")))
        lines = cfg.replace("\r\n", "\n").split("\n")
        for i, line in enumerate(lines):
            if line.strip() == "*KERNEL":
                lines.insert(i + 1, "ur0:tai/kubridge.skprx")
                break
        else:
            lines = ["*KERNEL", "ur0:tai/kubridge.skprx"] + lines
        ftp.storbinary("STOR " + cfg_path, io.BytesIO("\n".join(lines).encode("utf-8")))
        self.log("kubridge: added to %s (backup saved next to it). REBOOT the Vita before starting Brawl Vita." % cfg_path)


# ---------------------------------------------------------------- GUI
def removable_drives():
    if os.name != "nt":
        return []
    import ctypes
    out = []
    for letter in string.ascii_uppercase:
        root = "%s:\\" % letter
        if ctypes.windll.kernel32.GetDriveTypeW(root) == 2:   # DRIVE_REMOVABLE
            out.append(root)
    return out


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("BrawlVita installer %s" % VERSION)
        self.resizable(False, False)
        self.q = queue.Queue()
        pad = {"padx": 6, "pady": 3}
        r = 0
        ttk.Label(self, text="BrawlVita: Brawl Stars v29 for PS Vita", font=("Segoe UI", 13, "bold")).grid(row=r, column=0, columnspan=3, **pad)
        r += 1
        self.offline, self.original = tk.StringVar(), tk.StringVar()
        for label, var in (("Offline v29 APK", self.offline), ("Original v29 APK", self.original)):
            ttk.Label(self, text=label).grid(row=r, column=0, sticky="w", **pad)
            ttk.Entry(self, textvariable=var, width=52).grid(row=r, column=1, **pad)
            ttk.Button(self, text="Browse", command=lambda v=var: self.pick(v, [("APK", "*.apk"), ("All", "*.*")])).grid(row=r, column=2, **pad)
            r += 1
        self.profile = tk.StringVar(value="normal")
        fr = ttk.LabelFrame(self, text="Profile (each one is its own save)")
        fr.grid(row=r, column=0, columnspan=3, sticky="we", **pad)
        for val, txt in (("normal", "Normal - start from 0"), ("modded", "Modded - normal, infinite resources"),
                         ("max", "Max - everything unlocked and maxed")):
            ttk.Radiobutton(fr, text=txt, value=val, variable=self.profile).pack(anchor="w", padx=6)
        r += 1
        self.covers = {k: tk.StringVar() for k in COVERS}
        fc = ttk.LabelFrame(self, text="Cover art (optional; default: black with white BRAWLVITA)")
        fc.grid(row=r, column=0, columnspan=3, sticky="we", **pad)
        for i, (key, label) in enumerate((("icon", "Icon"), ("bg", "LiveArea background"), ("startup", "Start screen"))):
            ttk.Label(fc, text=label).grid(row=i, column=0, sticky="w", padx=6)
            ttk.Entry(fc, textvariable=self.covers[key], width=44).grid(row=i, column=1, padx=6, pady=2)
            ttk.Button(fc, text="Browse", command=lambda v=self.covers[key]: self.pick(v, [("Pictures", "*.png;*.jpg;*.jpeg;*.bmp;*.webp"), ("All", "*.*")])).grid(row=i, column=2, padx=6)
        r += 1
        self.download, self.no_music = tk.BooleanVar(value=True), tk.BooleanVar(value=False)
        ttk.Checkbutton(self, text="Download the high-resolution pictures (internet, about 51 MB)", variable=self.download).grid(row=r, column=0, columnspan=3, sticky="w", **pad)
        r += 1
        ttk.Checkbutton(self, text="Leave out music and sounds (saves about 29 MB)", variable=self.no_music).grid(row=r, column=0, columnspan=3, sticky="w", **pad)
        r += 1
        fo = ttk.LabelFrame(self, text="Put the files")
        fo.grid(row=r, column=0, columnspan=3, sticky="we", **pad)
        self.mode = tk.StringVar(value="folder")
        self.folder, self.usb, self.ip, self.port = tk.StringVar(), tk.StringVar(), tk.StringVar(), tk.StringVar(value="1337")
        self.kubridge = tk.BooleanVar(value=False)
        ttk.Radiobutton(fo, text="In a folder", value="folder", variable=self.mode).grid(row=0, column=0, sticky="w", padx=6)
        ttk.Entry(fo, textvariable=self.folder, width=36).grid(row=0, column=1, padx=6)
        ttk.Button(fo, text="Browse", command=lambda: self.folder.set(filedialog.askdirectory() or self.folder.get())).grid(row=0, column=2, padx=6)
        ttk.Radiobutton(fo, text="USB (VitaShell USB mode)", value="usb", variable=self.mode).grid(row=1, column=0, sticky="w", padx=6)
        self.usb_box = ttk.Combobox(fo, textvariable=self.usb, values=removable_drives(), width=33)
        self.usb_box.grid(row=1, column=1, padx=6)
        ttk.Button(fo, text="Refresh", command=lambda: self.usb_box.configure(values=removable_drives())).grid(row=1, column=2, padx=6)
        ttk.Radiobutton(fo, text="FTP (VitaShell, press SELECT)", value="ftp", variable=self.mode).grid(row=2, column=0, sticky="w", padx=6)
        ff = ttk.Frame(fo)
        ff.grid(row=2, column=1, sticky="w", padx=6)
        ttk.Label(ff, text="IP").pack(side="left")
        ttk.Entry(ff, textvariable=self.ip, width=16).pack(side="left", padx=4)
        ttk.Label(ff, text="port").pack(side="left")
        ttk.Entry(ff, textvariable=self.port, width=6).pack(side="left", padx=4)
        ttk.Checkbutton(fo, text="Also install the kubridge plugin over FTP (backs up your plugin list; reboot afterwards)",
                        variable=self.kubridge).grid(row=3, column=0, columnspan=3, sticky="w", padx=6)
        r += 1
        self.bar = ttk.Progressbar(self, length=560, maximum=100)
        self.bar.grid(row=r, column=0, columnspan=3, **pad)
        r += 1
        self.logbox = tk.Text(self, width=78, height=10, state="disabled")
        self.logbox.grid(row=r, column=0, columnspan=3, **pad)
        r += 1
        self.go = ttk.Button(self, text="Install", command=self.start)
        self.go.grid(row=r, column=0, columnspan=3, pady=8)
        self.after(100, self.pump)

    def pick(self, var, types):
        p = filedialog.askopenfilename(filetypes=types)
        if p:
            var.set(p)

    def log(self, text):
        self.q.put(("log", text))

    def progress(self, v):
        self.q.put(("bar", v))

    def pump(self):
        try:
            while True:
                kind, v = self.q.get_nowait()
                if kind == "log":
                    self.logbox.configure(state="normal")
                    self.logbox.insert("end", v + "\n")
                    self.logbox.see("end")
                    self.logbox.configure(state="disabled")
                elif kind == "bar":
                    self.bar["value"] = v
                elif kind == "done":
                    self.go.configure(state="normal")
                    (messagebox.showinfo if v[0] else messagebox.showerror)("BrawlVita", v[1])
        except queue.Empty:
            pass
        self.after(100, self.pump)

    def start(self):
        o = {"offline": self.offline.get(), "original": self.original.get(), "profile": self.profile.get(),
             "covers": {k: v.get() for k, v in self.covers.items()}, "download": self.download.get(), "no_music": self.no_music.get(),
             "mode": self.mode.get(), "folder": self.folder.get(), "usb": self.usb.get(), "ip": self.ip.get().strip(),
             "port": self.port.get().strip() or "1337", "kubridge": self.kubridge.get()}
        if not os.path.isfile(o["offline"]) or not os.path.isfile(o["original"]):
            messagebox.showerror("BrawlVita", "Choose both APK files first.")
            return
        if o["mode"] == "folder" and not os.path.isdir(o["folder"]):
            messagebox.showerror("BrawlVita", "Choose an output folder.")
            return
        if o["mode"] == "usb" and not o["usb"]:
            messagebox.showerror("BrawlVita", "Choose the Vita's drive (start VitaShell's USB mode first).")
            return
        if o["mode"] == "ftp" and not o["ip"]:
            messagebox.showerror("BrawlVita", "Enter the IP address VitaShell shows in FTP mode.")
            return
        self.go.configure(state="disabled")

        def work():
            try:
                Job(o, self.log, self.progress).run()
                self.q.put(("done", (True, "Finished. See the log for the last steps on the Vita.")))
            except Exception as e:
                self.log("ERROR: %s" % e)
                self.q.put(("done", (False, str(e))))

        threading.Thread(target=work, daemon=True).start()


if __name__ == "__main__":
    App().mainloop()
