#!/usr/bin/env python3
"""End-to-end test of the installer: from an empty machine to a booted system.

    tests/installer/e2e.py SCENARIO [--iso ISO] [--disk-if nvme|sata|virtio]
    tests/installer/e2e.py --explore SCENARIO     boot the live ISO and stop there
    tests/installer/e2e.py --list

For a scenario:
1. mkdisk.sh makes its disks; the partitions to keep are noted (table entry,
   file system UUID, a hash of their first 64 MiB).
2. The live ISO boots in QEMU and the installer is driven through its window
   as a person would, by QMP pointer and keys at 1280x800, waiting on what
   the screen shows; a screenshot after each step.
3. Once Anaconda and the post-install are done the logs are kept, Restart
   is clicked and the VM must restart (issue #89), and the kept partitions
   are compared.
4. The disks boot alone: the GRUB menu is captured, the new account logs in
   over SSH and the system is checked (Windows in the menu where expected).

Results: out/e2e/<scenario>/ (screenshots, logs, result.txt).
Needs qemu-system-x86_64 with KVM, OVMF, qemu-nbd, sshpass and what
mkdisk.sh needs, sudo for the disks. Environment:
  E2E_VNC=127.0.0.1:1   the VM's display, to watch it (a VNC or noVNC client)
  E2E_SSH_PORT=2224     the live and installed systems' SSH, on the host
"""
import argparse, hashlib, json, os, shlex, socket, subprocess, sys, time

ROOT = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))
HERE = os.path.join(ROOT, "tests", "installer")
OUT = os.path.join(ROOT, "out", "e2e")
W, H = 1280, 800
LIVE = ("igos", "igos")  # builder/profiles/igos: the live session's account
USER, PASSWORD = "tester", "ujicoba-bayu-2026"
VNC = os.environ.get("E2E_VNC", "127.0.0.1:1")
SSH_PORT = os.environ.get("E2E_SSH_PORT", "2224")

# disks: (mkdisk.sh scenario, size); keep: (disk, partition) left as they are;
# windows: whether GRUB must list Windows; flow: how the installer is used.
SCENARIOS = {
    "blank": dict(disks=[("blank", "40G")], keep=[], windows=False, flow="default"),
    "blank-bios": dict(disks=[("blank", "40G")], keep=[], windows=False, flow="default", bios=True),
    "luks": dict(disks=[("blank", "40G")], keep=[], windows=False, flow="encrypt"),
    "win-free": dict(disks=[("win-free", None)], keep=[(0, 1), (0, 2), (0, 3), (0, 4)], windows=True, flow="default"),
    "win-free-sata": dict(disks=[("win-free", None)], keep=[(0, 1), (0, 2), (0, 3), (0, 4)], windows=True,
                          flow="default", disk_if="sata"),
    "win-esp200": dict(disks=[("win-esp200", None)], keep=[(0, 1), (0, 2), (0, 3), (0, 4)], windows=True,
                       flow="default"),
    # The EFI partition kept is full: the review offers a new one, accepted.
    "win-esp-full": dict(disks=[("win-esp-full", None)], keep=[(0, 1), (0, 2), (0, 3), (0, 4)], windows=True,
                         flow="new-esp"),
    "win-full": dict(disks=[("win-full", None)], keep=[(0, 1), (0, 2)], windows=True, flow="alongside"),
    "win-mbr": dict(disks=[("win-mbr", None)], keep=[(0, 1), (0, 2)], windows=True, flow="default", bios=True),
    "two-disks": dict(disks=[("win-full", None), ("blank", "40G")], keep=[(0, 1), (0, 2), (0, 3)], windows=True,
                      flow="second-disk"),
    "custom-free": dict(disks=[("win-free", None)], keep=[(0, 1), (0, 2), (0, 3), (0, 4)], windows=True,
                        flow="custom"),
    # Issue #65: Windows, another Linux, free space in the middle, data after it.
    "triple-mid": dict(disks=[("triple-mid", None)], keep=[(0, n) for n in range(1, 7)], windows=True,
                       flow="default"),
    # Next to Arch Linux (GRUB, or systemd-boot with the EFI partition at /boot):
    # the GRUB IGN installs must boot, and list Arch through os-prober.
    "arch-grub": dict(disks=[("arch-grub", None)], keep=[(0, 1), (0, 2)], windows=True, flow="default"),
    "arch-sdboot": dict(disks=[("arch-sdboot", None)], keep=[(0, 1), (0, 2)], windows=True, flow="default"),
    "too-small": dict(disks=[("blank", "4G")], keep=[], windows=False, flow="refused"),
}


def run(cmd, check=True, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), check=check, text=True, capture_output=True, **kw)


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(50):
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.2)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def close(self):
        self.s.close()

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args} if args else {"execute": name}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r

    def move(self, x, y):
        self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / H)}}])

    def click(self, x, y):
        # A motion first: the pointer may be there already, unknown to the app.
        self.move(x + 3, y + 3); time.sleep(0.08)
        self.move(x, y); time.sleep(0.15)
        for down in (True, False):
            self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
            time.sleep(0.06)
        time.sleep(0.6)

    def key(self, *names):
        for down in (True, False):
            for n in (names if down else reversed(names)):
                self.cmd("input-send-event",
                         events=[{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": n}}}])
            time.sleep(0.03)

    def type(self, text):
        special = {" ": ("spc",), "-": ("minus",), ".": ("dot",), "/": ("slash",)}
        for c in text:
            self.key(*special[c]) if c in special else self.key("shift", c.lower()) if c.isupper() else self.key(c)
        time.sleep(0.3)

    def shot(self, path):
        self.cmd("screendump", filename=path, format="png")

    def screen(self):
        """The screen as (width, height, RGB bytes), from a PPM screendump."""
        path = f"/tmp/bayu-e2e-{os.getpid()}.ppm"
        self.cmd("screendump", filename=path)
        data = open(path, "rb").read()
        os.unlink(path)
        # QEMU writes "P6\n<width> <height>\n255\n" and the pixels.
        magic, size, depth, rgb = data.split(b"\n", 3)
        w, h = map(int, size.split())
        return w, h, rgb

    def pixel(self, x, y):
        w, h, rgb = self.screen()
        i = (y * w + x) * 3
        return tuple(rgb[i:i + 3])

    def region(self, x0, y0, x1, y1):
        w, h, rgb = self.screen()
        return hashlib.sha1(b"".join(rgb[(y * w + x0) * 3:(y * w + x1) * 3] for y in range(y0, y1))).hexdigest()

    def wait(self, test, seconds=90):
        end = time.time() + seconds
        while time.time() < end:
            if test():
                return True
            time.sleep(1)
        return False


class Vm:
    """QEMU with the live ISO and/or the disks; one at a time."""

    def __init__(self, out, disks, disk_if, bios):
        self.out, self.disks, self.disk_if, self.bios = out, disks, disk_if, bios
        self.qmp_path = os.path.join(out, "qmp.sock")
        self.vars = os.path.join(out, "vars.fd")
        self.proc = None
        self.qmp = None

    def start(self, iso=None):
        run("pkill -f '^qemu-system-x86_64 -name bayu-e2e'; sleep 2", check=False)
        if not self.bios and not os.path.exists(self.vars):
            run(["cp", "/usr/share/OVMF/OVMF_VARS_4M.fd", self.vars])
        drives = []
        for i, disk in enumerate(self.disks):
            boot = "" if iso else f",bootindex={i + 1}"
            drives += ["-drive", f"file={disk},if=none,id=d{i},format=qcow2", "-device", {
                "virtio": f"virtio-blk-pci,drive=d{i}{boot}",
                "sata": f"ide-hd,drive=d{i},bus=ide.{i}{boot}",
                "nvme": f"nvme,drive=d{i},serial=disk{i}{boot}",
            }[self.disk_if]]
        fw = [] if self.bios else [
            "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
            "-drive", f"if=pflash,format=raw,file={self.vars}"]
        cd = ["-cdrom", iso, "-boot", "order=dc"] if iso else []
        if iso and iso.endswith(".img"):
            # A USB stick image, such as Ventoy's with the ISO on it; left unchanged.
            cd = ["-drive", f"file={iso},if=none,id=usb,format=raw,snapshot=on"]
        cmd = ["qemu-system-x86_64", "-name", "bayu-e2e", "-machine", "q35,accel=kvm", "-cpu", "host",
               "-smp", "4", "-m", "6G", *fw, *cd, *drives,
               "-nic", f"user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:{SSH_PORT}-:22",
               "-device", "virtio-vga", "-display", "none", "-vnc", VNC,
               "-device", "qemu-xhci,id=xhci", "-device", "usb-tablet",
               *(["-device", "usb-storage,bus=xhci.0,drive=usb,bootindex=0"] if iso and iso.endswith(".img") else []),
               "-serial", f"file:{self.out}/serial.log", "-qmp", f"unix:{self.qmp_path},server,nowait",
               # A restart ends QEMU, so that the installer's Restart can be told.
               "-no-reboot"]
        self.proc = subprocess.Popen(cmd, stdout=open(f"{self.out}/qemu.log", "a"), stderr=subprocess.STDOUT)
        time.sleep(1)
        self.qmp = Qmp(self.qmp_path)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.qmp.cmd("quit")
            self.proc.wait(timeout=30)
        self.proc = None

    def ssh(self, command, account=LIVE, check=False, timeout=240):
        try:
            return run(["sshpass", "-p", account[1], "ssh", "-o", "StrictHostKeyChecking=no",
                        "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=3",
                        "-p", SSH_PORT, f"{account[0]}@127.0.0.1", command], check=check, timeout=timeout)
        except subprocess.TimeoutExpired:
            return subprocess.CompletedProcess(command, 124, "", "timed out")

    def wait_ssh(self, account=LIVE, seconds=300):
        end = time.time() + seconds
        while time.time() < end:
            if self.ssh("true", account, timeout=10).returncode == 0:
                return True
            time.sleep(3)
        return False


def partitions(disk):
    """Partition number -> (start end type, UUID, hash of the first 64 MiB, name)."""
    run("sudo modprobe nbd max_part=16")
    # A free device: one left by a killed run may hang.
    dev = next(f"/dev/nbd{i}" for i in range(16) if not os.path.exists(f"/sys/block/nbd{i}/pid"))
    run(["sudo", "qemu-nbd", "-r", "-c", dev, disk])
    try:
        for _ in range(50):
            if run(["sudo", "blockdev", "--getsize64", dev], check=False).stdout.strip() not in ("", "0"):
                break
            time.sleep(0.2)
        run(["sudo", "partprobe", dev], check=False)
        run("sudo udevadm settle")
        table = json.loads(run(["sudo", "sfdisk", "-J", dev], check=False).stdout or '{"partitiontable": {}}')
        rows = {}
        for p in table["partitiontable"].get("partitions", []):
            n = int(p["node"].rsplit("p", 1)[1])
            uuid = run(["sudo", "blkid", "-s", "UUID", "-o", "value", p["node"]], check=False).stdout.strip()
            head = run(f"sudo dd if={p['node']} bs=1M count=64 status=none | sha256sum").stdout.split()[0]
            rows[n] = (f"{p['start']} {p['size']} {p['type']}", uuid, head, p.get("name", ""))
        return rows
    finally:
        run(["sudo", "qemu-nbd", "-d", dev], check=False)


class Test:
    # The installer's window, maximized at 1280x800.
    NEXT = (1085, 730)
    INSTALL = (1068, 730)

    def __init__(self, name, iso, disk_if=None):
        self.name, self.iso = name, iso
        self.spec = SCENARIOS[name]
        self.out = os.path.join(OUT, name)
        run(["sudo", "rm", "-rf", self.out])
        os.makedirs(self.out)
        self.disks = [os.path.join(self.out, f"disk{i}.qcow2") for i in range(len(self.spec["disks"]))]
        self.steps = 0
        self.notes = []
        self.ok = True
        disk_if = disk_if or self.spec.get("disk_if") or ("virtio" if self.spec.get("bios") else "nvme")
        self.vm = Vm(self.out, self.disks, disk_if, self.spec.get("bios", False))

    def note(self, ok, text):
        self.notes.append(("PASS " if ok else "FAIL ") + text)
        print(self.notes[-1], flush=True)
        self.ok &= ok

    def shot(self, name):
        self.steps += 1
        self.vm.qmp.shot(f"{self.out}/{self.steps:02d}-{name}.png")

    def ready(self):
        """The primary button (Continue, Start installation) is enabled: blue."""
        # Inside both buttons, clear of their labels.
        r, g, b = self.vm.qmp.pixel(1025, 742)
        return b > 200 and r < 120

    def next(self, page):
        """Continues once the page allows it, and waits for the next page."""
        q = self.vm.qmp
        if not q.wait(self.ready):
            raise RuntimeError(f"cannot continue to {page}")
        title = q.region(400, 92, 1000, 140)
        q.click(*self.NEXT)
        if not q.wait(lambda: q.region(400, 92, 1000, 140) != title, 60):
            raise RuntimeError(f"{page} did not open")
        time.sleep(2)
        self.shot(page)

    def prepare(self):
        for (kind, size), disk in zip(self.spec["disks"], self.disks):
            run([os.path.join(HERE, "mkdisk.sh"), kind, disk] + ([size] if size else []))
        self.before = [partitions(d) for d in self.disks]
        missing = [k for k in self.spec["keep"] if k[1] not in self.before[k[0]]]
        if missing:
            raise RuntimeError(f"partitions {missing} were not made")

    def flow(self, kind):
        """Walks the installer to the start of the installation; False where it
        must refuse to go on, and does."""
        q = self.vm.qmp
        # The installer opens on its own: the language page, ready.
        if not q.wait(self.ready, 180):
            raise RuntimeError("the installer did not open")
        self.shot("language")
        for page in ("network", "localization", "destination"):
            self.next(page)
        q.wait(self.ready, 30)
        if kind == "refused":
            time.sleep(5)
            self.shot("refused")
            return self.ready()
        if kind == "alongside":
            q.click(540, 456); time.sleep(2); q.wait(self.ready); self.shot("alongside")
        elif kind == "second-disk":
            # The empty disk is the one picked: its card is tinted, not white.
            r, g, b = q.pixel(650, 355)
            if not b > r + 10:
                raise RuntimeError("the empty disk is not the one picked")
            self.shot("second-disk")
        elif kind == "custom":
            # The editor: the free space is picked first; the recommended
            # layout goes in it.
            q.click(975, 340); time.sleep(2)
            self.next("editor")
            q.click(590, 392); time.sleep(4)
            self.shot("custom-layout")
        elif kind == "encrypt":
            q.click(1113, 491); time.sleep(2); q.wait(self.ready); self.shot("encrypt")
        self.next("software")
        self.next("account")
        q.click(590, 327); q.type(USER)
        q.click(570, 487); q.type(PASSWORD)
        q.click(570, 567); q.type(PASSWORD)
        self.shot("account-filled")
        self.next("review")
        if kind == "new-esp":
            # The check finds the EFI partition full: the offer, accepted.
            time.sleep(8)
            self.shot("esp-full")
            q.key("ret"); time.sleep(8)
            self.shot("new-esp")
        if not q.wait(self.ready):
            raise RuntimeError("cannot start the installation")
        q.click(*self.INSTALL)
        return True

    def wait_install(self, seconds=1800):
        """Waits for Anaconda's installation task and the post-install: '' or the error."""
        end = time.time() + seconds
        task = "sudo journalctl -b -o cat --no-pager | grep -m1 'nabau: task .*/Boss/Task/[0-9]* finished'"
        while time.time() < end:
            line = self.vm.ssh(task).stdout.strip()
            if line:
                # "... finished" or "... finished: <error>"
                error = line.split("finished", 1)[1].lstrip(":").strip()
                if error:
                    return error
                time.sleep(20)
                for _ in range(300):
                    if self.vm.ssh("pgrep -f /usr/libexec/nabau/post-install").returncode != 0:
                        return ""
                    time.sleep(3)
                return "the post-install did not end"
            time.sleep(10)
        return "timed out"

    def boot(self):
        """Boots the installed disks; where they are encrypted, the account's
        password opens them."""
        self.vm.start()
        for i in range(10):
            time.sleep(2)
            self.shot(f"boot{i}")
        if self.spec["flow"] == "encrypt":
            time.sleep(15)
            self.shot("passphrase-prompt")
            self.vm.qmp.type(PASSWORD)
            self.vm.qmp.key("ret")
        return self.vm.wait_ssh((USER, PASSWORD), 400)

    def main(self):
        self.prepare()
        self.vm.start(self.iso)
        if not self.vm.wait_ssh():
            self.note(False, "the live system comes up")
            return self.finish()
        try:
            go = self.flow(self.spec["flow"])
        except RuntimeError as e:
            self.shot("stuck")
            self.note(False, str(e))
            return self.finish()
        if self.spec["flow"] == "refused":
            self.note(not go, "the installer refuses the disk")
            return self.finish()

        error = self.wait_install()
        time.sleep(3)
        self.shot("result")
        self.vm.qmp.click(900, 716); time.sleep(2)
        self.shot("log")
        self.vm.qmp.key("esc")
        for log in ("nabau-install.log", "anaconda.log", "storage.log", "nabau-post.log", "nabau-backend.log"):
            open(f"{self.out}/{log}", "w").write(self.vm.ssh(f"sudo cat /tmp/{log}").stdout)
        self.note(not error, "installation" + (f": {error}" if error else ""))
        if error:
            return self.finish()
        # Restart now: the computer restarts, Nabau does not only close. The
        # log's window goes first, by its close button.
        self.vm.qmp.click(975, 180); time.sleep(2)
        self.shot("before-restart")
        self.vm.qmp.click(1043, 730)
        restarted = False
        for _ in range(60):
            time.sleep(3)
            if self.vm.proc.poll() is not None:
                restarted = True
                break
        if not restarted:
            self.shot("restart-stuck")
            open(f"{self.out}/nabau-backend-restart.log", "w").write(
                self.vm.ssh("sudo cat /tmp/nabau-backend.log; echo ===; ps -ef | grep -E 'nabau|anaconda' | grep -v grep").stdout)
        self.note(restarted, "Restart restarts the computer")
        self.vm.stop()

        after = [partitions(d) for d in self.disks]
        for disk, n in self.spec["keep"]:
            row = self.before[disk][n]
            # The EFI system partition gets the new boot loader beside Windows'.
            esp = "C12A7328" in row[0].upper()
            same = after[disk].get(n, ())[:2] == row[:2] if esp else after[disk].get(n) == row
            self.note(same, f"disk {disk} partition {n} kept ({row[3] or row[0]})")

        if not self.boot():
            self.shot("no-login")
            self.note(False, f"{USER} logs in")
            return self.finish()
        self.note(True, f"{USER} logs in")
        sh = lambda c: self.vm.ssh(c, (USER, PASSWORD)).stdout.strip()
        sudo = lambda c: sh(f"echo {shlex.quote(PASSWORD)} | sudo -S -p '' {c}")
        state = sh("systemctl is-system-running --wait")
        self.note(state in ("running", "degraded"), f"system state: {state}")
        if state != "running":
            self.notes.append("INFO failed units: " + sh("systemctl --failed --no-legend --plain | cut -d' ' -f1"))
        menu = sudo("grep -c osprober /boot/grub2/grub.cfg")
        windows = menu.isdigit() and int(menu) > 0
        self.note(windows == self.spec["windows"], f"Windows in the GRUB menu: {windows}")
        if self.spec["flow"] == "new-esp":
            esp = sh("findmnt -no SOURCE /boot/efi")
            self.note(esp and not esp.endswith("1"), f"a new EFI partition: {esp}")
        binder = sh("grep -c '^CONFIG_ANDROID_BINDER_IPC=y' /boot/config-$(uname -r)")
        self.note(binder == "1", "kernel with binder")
        open(f"{self.out}/system.txt", "w").write(sh(
            "uname -r; lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINTS; head -2 /etc/os-release"))
        # What GRUB offers next to IGN, and the firmware's boot entries.
        open(f"{self.out}/boot.txt", "w").write(sudo(
            "sh -c 'sed -n \"/30_os-prober/,/END .*30_os-prober/p\" /boot/grub2/grub.cfg; echo ===; "
            "efibootmgr -v; echo ===; find /boot/efi -maxdepth 4 | sort'"))
        self.shot("installed")
        return self.finish()

    def finish(self):
        self.vm.stop()
        result = ("PASS" if self.ok else "FAIL") + f" {self.name}\n" + "\n".join(self.notes) + "\n"
        open(f"{self.out}/result.txt", "w").write(result)
        print(result, flush=True)
        # An installed disk takes gigabytes: kept only to look into a failure.
        if self.ok:
            for disk in self.disks:
                if os.path.exists(disk):
                    os.unlink(disk)
        return 0 if self.ok else 1


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("scenarios", nargs="*", help="default: all")
    p.add_argument("--iso", default=os.path.join(ROOT, "out", "igos-bayu-live.iso"))
    p.add_argument("--disk-if", choices=["nvme", "sata", "virtio"])
    p.add_argument("--explore", action="store_true", help="boot the live ISO with the disks and leave it running")
    p.add_argument("--list", action="store_true")
    a = p.parse_args()
    if a.list:
        print("\n".join(SCENARIOS))
        return 0
    names = a.scenarios or list(SCENARIOS)
    unknown = [n for n in names if n not in SCENARIOS]
    if unknown:
        p.error(f"unknown scenarios: {' '.join(unknown)}")
    if a.explore:
        t = Test(names[0], a.iso, a.disk_if)
        t.prepare()
        t.vm.start(a.iso)
        t.vm.qmp.close()
        print(f"running: QMP {t.vm.qmp_path}, SSH port {SSH_PORT}, VNC {VNC}")
        return 0
    failed = []
    for name in names:
        t = Test(name, a.iso, a.disk_if)
        try:
            if t.main():
                failed.append(name)
        except Exception as e:
            t.note(False, f"{type(e).__name__}: {e}")
            t.finish()
            failed.append(name)
    print(f"{len(names) - len(failed)}/{len(names)} passed" + (f"; failed: {' '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
