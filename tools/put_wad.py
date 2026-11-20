#!/usr/bin/env python3
"""把 WAD 写进 CS80 外挂 flash 的 EFS:/monita/（monita-doom 从那里读）。

前提：设备上跑的是 extdump v2（它的固件只许写 EFS:/monita/ 下面、写类命令要先 ARM，见 cs80-extflash/README-v2.md）。
本脚本只是调 extdump v2 的主机客户端：HELLO → INIT 挂载 → 查剩余空间 → ARM → mkdir → 原子写（.part 再改名）
→ 整个读回比 sha256。不碰 EFS:/monita/ 以外的任何文件。

  python3 tools/put_wad.py --port COM5 DOOM1.WAD            # ThinkPad（pyserial，上次 extdump v2 走的这条）
  python3 tools/put_wad.py --port usb  DOOM1.WAD            # Mac（pyusb 直开接口 7，extdump 那边注明未上机验证）
  环境变量 EXTFLASH_HOST 必须指向 extdump v2 工具的 host/ 目录（extlink.py、extflash_v2.py 所在处）
设备上的文件名：共享版 / 原版 → doom1.wad；Freedoom Phase 1 → freedoom1.wad（--name 可改，只能 [a-z0-9._-]）。
"""
import argparse, hashlib, os, sys, time

HOST = os.environ.get("EXTFLASH_HOST", "")
RESERVE = 256 * 1024          # 同 extdump v2 固件的保留线：写后剩余空间不得低于它


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wad")
    ap.add_argument("--port", required=True, help="COMx（ThinkPad）或 usb（Mac）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--name", default=None, help="设备上的文件名，默认按 WAD 内容猜 doom1.wad / freedoom1.wad")
    ap.add_argument("--timeout", type=float, default=3.0)
    ap.add_argument("--no-verify", action="store_true", help="不整个读回（读回约 74 KB/s，4 MB 要一分钟）")
    a = ap.parse_args()

    data = open(a.wad, "rb").read()
    if data[:4] not in (b"IWAD", b"PWAD"):
        sys.exit("%s 不是 WAD（开头不是 IWAD / PWAD）" % a.wad)
    name = a.name or ("freedoom1.wad" if b"FREEDOOM" in data[:1 << 20].upper() else "doom1.wad")
    path = "EFS:/monita/" + name
    sha = hashlib.sha256(data).hexdigest()
    print("WAD %s：%d B，sha256 %s → %s" % (a.wad, len(data), sha[:16], path))

    if not os.path.isfile(os.path.join(HOST, "extflash_v2.py")):
        sys.exit("要设 EXTFLASH_HOST=<extdump v2 工具>/host")
    sys.path.insert(0, HOST)
    import extlink as L                                  # noqa: E402
    from extflash_v2 import Client                       # noqa: E402
    c = Client(L.Link(L.open_transport(a.port, a.baud), a.timeout))
    print("HELLO：", c.hello())
    print("INIT：", c.init())
    info = c.info()
    old = c.stat(path)
    # 写 .part 的时候旧文件还在（写完才删旧、改名），所以要 剩余 - 新文件 >= 保留线（同 monita-x86 的 scripts/efs_put.py）
    free_during = info["free"] - len(data)
    print("EFS 剩余 %.2f MiB；写 .part 时最少剩 %.2f MiB，写完约剩 %.2f MiB（保留线 %d KiB）" % (
        info["free"] / 1048576, free_during / 1048576, (free_during + (old or 0)) / 1048576, RESERVE >> 10))
    if free_during < RESERVE:
        sys.exit("空间不够：写 .part 时新旧两份同时在，会低于保留线，停手")
    c.arm()
    c.mkdir("EFS:/monita")                               # 已存在也无妨（固件只回 ret）
    t0 = time.time()
    fs = c.put_file(path, data)
    dt = time.time() - t0
    print("写完：%.1f s（%.1f KiB/s），fsync=%d，设备上大小 %s" % (dt, len(data) / 1024 / dt, fs, c.stat(path)))
    if not a.no_verify:
        t0 = time.time()
        back = c.get_file(path)
        ok = hashlib.sha256(back).hexdigest() == sha
        print("读回 %.1f s：%s" % (time.time() - t0, "sha256 一致" if ok else "!! 不一致"))
        if not ok:
            sys.exit(1)
    print("完成。接着刷 build/r05/doom.bin 到 0x60270000。")


if __name__ == "__main__":
    main()
