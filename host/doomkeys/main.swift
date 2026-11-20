// doomkeys：把 Mac 键盘转发给跑 monita-doom 的 CS80（USB 接口 7 直连），设备日志打到终端。
//   swiftc -O -o build/doomkeys host/doomkeys/*.swift    （或 sh host/doomkeys/build.sh）
//   build/doomkeys              开一个小窗口，窗口在前台时按键转发；Ctrl-C 退出
//   build/doomkeys --interface N   指定 USB 接口号（默认 7）
//   build/doomkeys --selftest      只跑自检（帧编码 / 键位表），不碰 USB
// 协议见 port/r05/dg_link.h：主机发 HELLO 0x01 / PING 0x7F（每 250 ms）/ KEY 0xA0 (u8 键, u8 按下)，
// 设备回 HELLO_ACK 0x81 ("DOOM", u16 版本, u16 flags) 和 LOG 0xA8（文本）。
// 键位：方向键走 / 转，Z 或 Ctrl 开火，空格开门，Shift 跑，Option 平移，Esc 菜单，回车确认，数字换枪，
//       Tab 地图，F1–F12 同原版。按住 Command 的组合键不转发。窗口失焦时补发全部抬起。
//       （macOS 默认 Ctrl+方向键切桌面、Ctrl+空格切输入法，会被系统截走，所以边走边开火用 Z。Z 不在任何秘籍里。）
//
// Copyright (C) 2026 Monita-CS80 维护者
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import Foundation

// ---- 帧 ----
let crcTable: [UInt32] = (0..<256).map { i -> UInt32 in
    var c = UInt32(i)
    for _ in 0..<8 { c = (c & 1) != 0 ? (c >> 1) ^ 0xEDB8_8320 : c >> 1 }
    return c
}
func crc32(_ b: ArraySlice<UInt8>) -> UInt32 {
    var c: UInt32 = 0xFFFF_FFFF
    for x in b { c = (c >> 8) ^ crcTable[Int((c ^ UInt32(x)) & 0xFF)] }
    return ~c
}
func encodeFrame(_ type: UInt8, _ payload: [UInt8] = []) -> [UInt8] {
    var f: [UInt8] = [0xA5, 0x5A, type, 0, UInt8(payload.count & 0xFF), UInt8(payload.count >> 8)] + payload
    let c = crc32(f[2...])
    f += [UInt8(c & 0xFF), UInt8((c >> 8) & 0xFF), UInt8((c >> 16) & 0xFF), UInt8(c >> 24)]
    return f
}
/// 流式拆帧：喂字节，吐出 (type, payload)；坏 CRC 的丢掉、往后一个字节重新找同步字
struct Parser {
    var buf: [UInt8] = []
    mutating func feed(_ d: [UInt8]) -> [(UInt8, [UInt8])] {
        buf += d
        var out: [(UInt8, [UInt8])] = []
        while true {
            guard let i = buf.indices.first(where: { $0 + 1 < buf.count && buf[$0] == 0xA5 && buf[$0 + 1] == 0x5A }) else {
                if buf.last == 0xA5 { buf = [0xA5] } else { buf = [] }
                return out
            }
            if i > 0 { buf.removeFirst(i) }
            guard buf.count >= 6 else { return out }
            let n = Int(buf[4]) | Int(buf[5]) << 8
            guard buf.count >= 10 + n else { return out }
            let c = UInt32(buf[6 + n]) | UInt32(buf[7 + n]) << 8 | UInt32(buf[8 + n]) << 16 | UInt32(buf[9 + n]) << 24
            if crc32(buf[2..<(6 + n)]) == c {
                out.append((buf[2], Array(buf[6..<(6 + n)])))
                buf.removeFirst(10 + n)
            } else {
                buf.removeFirst(1)
            }
        }
    }
}

// ---- 键位：macOS keyCode -> Doom 键码 (doomgeneric/doomkeys.h) ----
let KEY_RSHIFT: UInt8 = 0x80 + 0x36, KEY_RCTRL: UInt8 = 0x80 + 0x1D, KEY_RALT: UInt8 = 0x80 + 0x38
let KEY_USE: UInt8 = 0xA2, KEY_FIRE: UInt8 = 0xA3
let specialKeys: [UInt16: UInt8] = [
    123: 0xAC, 124: 0xAE, 125: 0xAF, 126: 0xAD,                 // ← → ↓ ↑
    36: 13, 76: 13, 53: 27, 48: 9, 51: 0x7F, 49: KEY_USE,       // 回车 / 小键盘回车 / Esc / Tab / Delete / 空格
    6: KEY_FIRE,                                                // Z
    122: 0x80 + 0x3B, 120: 0x80 + 0x3C, 99: 0x80 + 0x3D, 118: 0x80 + 0x3E, 96: 0x80 + 0x3F, 97: 0x80 + 0x40,
    98: 0x80 + 0x41, 100: 0x80 + 0x42, 101: 0x80 + 0x43, 109: 0x80 + 0x44, 103: 0x80 + 0x57, 111: 0x80 + 0x58,   // F1–F12
]
/// 修饰键（flagsChanged）：左右 Ctrl = 开火，Shift = 跑，Option = 平移
let modifierKeys: [UInt16: (UInt8, NSEvent.ModifierFlags)] = [
    59: (KEY_FIRE, .control), 62: (KEY_FIRE, .control),
    56: (KEY_RSHIFT, .shift), 60: (KEY_RSHIFT, .shift),
    58: (KEY_RALT, .option), 61: (KEY_RALT, .option),
]
func doomKey(keyCode: UInt16, chars: String?) -> UInt8? {
    if let k = specialKeys[keyCode] { return k }
    guard let s = chars?.lowercased().unicodeScalars.first, s.value >= 0x21, s.value < 0x7F else { return nil }
    return UInt8(s.value)
}

// ---- 自检 ----
func selftest() -> Int32 {
    var fails = 0
    func check(_ c: Bool, _ m: String) { if !c { fails += 1; print("FAIL \(m)") } }
    // 和 port/r05/dg_link.h + monita/proto.h 同一个 CRC-32/IEEE；"123456789" 的标准校验值
    check(crc32(Array("123456789".utf8)[...]) == 0xCBF4_3926, "crc32 标准值")
    let f = encodeFrame(0xA0, [0xAD, 1])
    check(f.count == 12 && f[0] == 0xA5 && f[1] == 0x5A && f[2] == 0xA0 && f[4] == 2 && f[5] == 0, "KEY 帧头")
    var p = Parser()
    let got = p.feed([0x00, 0xA5] + encodeFrame(0xA8, Array("hi\n".utf8)) + Array(encodeFrame(0x81, [1, 2]).prefix(5)))
    check(got.count == 1 && got[0].0 == 0xA8 && got[0].1 == Array("hi\n".utf8), "拆 LOG 帧（前面有垃圾）")
    let rest = p.feed(Array(encodeFrame(0x81, [1, 2]).dropFirst(5)))
    check(rest.count == 1 && rest[0].0 == 0x81 && rest[0].1 == [1, 2], "跨两次喂的帧")
    var bad = encodeFrame(0xA8, [1]); bad[7] ^= 1
    check(p.feed(bad + encodeFrame(0xA8, [2])).map { $0.1 } == [[2]], "坏 CRC 丢掉、后面的照收")
    check(doomKey(keyCode: 126, chars: nil) == 0xAD && doomKey(keyCode: 49, chars: " ") == KEY_USE, "↑ / 空格")
    check(doomKey(keyCode: 6, chars: "z") == KEY_FIRE, "Z = 开火")
    check(doomKey(keyCode: 16, chars: "Y") == UInt8(ascii: "y") && doomKey(keyCode: 18, chars: "1") == UInt8(ascii: "1"), "字母转小写 / 数字")
    check(doomKey(keyCode: 111, chars: nil) == 0x80 + 0x58 && doomKey(keyCode: 999, chars: "é") == nil, "F12 / 非 ASCII")
    print(fails == 0 ? "doomkeys 自检: OK" : "doomkeys 自检: \(fails) FAIL")
    return fails == 0 ? 0 : 1
}

// ---- USB 链路 ----
final class Link {
    let iface: Int
    private var port: UsbPort?
    private let q = DispatchQueue(label: "doomkeys.usb")
    private var held = Set<UInt8>()
    private var lastErr = ""
    var status: ((String) -> Void)?

    init(iface: Int) { self.iface = iface }

    func start() {
        Thread { self.readLoop() }.start()
        Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { _ in self.q.async { self.send(encodeFrame(0x7F)) } }
    }

    private func say(_ s: String) { if s != lastErr { lastErr = s; print("[doomkeys] \(s)"); DispatchQueue.main.async { self.status?(s) } } }

    /// 在 q 上调
    private func send(_ f: [UInt8]) {
        if port == nil {
            let (p, why) = UsbPort.open(interface: iface)
            guard let p else { say("连不上：\(why)"); return }
            port = p
            say("已连上接口 \(iface)，发 HELLO")
            _ = p.write(encodeFrame(0x01))
        }
        if port?.write(f) != true { port = nil; say("USB 断了，重连中") }
    }

    func key(_ k: UInt8, down: Bool) {
        q.async {
            if down { if self.held.contains(k) { return }; self.held.insert(k) } else { if self.held.remove(k) == nil { return } }
            self.send(encodeFrame(0xA0, [k, down ? 1 : 0]))
        }
    }
    func releaseAll() { q.async { for k in self.held { self.send(encodeFrame(0xA0, [k, 0])) }; self.held.removeAll() } }

    private func readLoop() {
        var parser = Parser()
        while true {
            guard let p = q.sync(execute: { port }) else { Thread.sleep(forTimeInterval: 0.2); continue }
            guard let d = p.read(timeoutMs: 200) else { q.sync { if port === p { port = nil } }; parser = Parser(); continue }
            for (t, pl) in parser.feed(d) {
                if t == 0xA8 { FileHandle.standardOutput.write(Data(pl)) }
                else if t == 0x81, pl.count >= 8, pl[0..<4] == [0x44, 0x4F, 0x4F, 0x4D] {
                    let flags = Int(pl[6]) | Int(pl[7]) << 8
                    say("设备是 monita-doom v\(Int(pl[4]) | Int(pl[5]) << 8)（\(flags & 1 != 0 ? "有声版" : "无声版")）")
                }
            }
        }
    }
}

// ---- 窗口 ----
final class KeyView: NSView {
    var link: Link!
    var mods = Set<UInt16>()
    override var acceptsFirstResponder: Bool { true }
    override func keyDown(with e: NSEvent) {
        if e.isARepeat || e.modifierFlags.contains(.command) { return }
        if let k = doomKey(keyCode: e.keyCode, chars: e.charactersIgnoringModifiers) { link.key(k, down: true) }
    }
    override func keyUp(with e: NSEvent) {
        if let k = doomKey(keyCode: e.keyCode, chars: e.charactersIgnoringModifiers) { link.key(k, down: false) }
    }
    override func flagsChanged(with e: NSEvent) {
        guard let (k, flag) = modifierKeys[e.keyCode] else { return }
        link.key(k, down: e.modifierFlags.contains(flag))
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    let link: Link
    var window: NSWindow!
    init(link: Link) { self.link = link }
    func applicationDidFinishLaunching(_ n: Notification) {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 420, height: 140), styleMask: [.titled, .closable, .miniaturizable],
                          backing: .buffered, defer: false)
        window.title = "doomkeys：按键转发到 CS80"
        let v = KeyView(frame: window.contentRect(forFrameRect: window.frame))
        v.link = link
        let label = NSTextField(labelWithString: "窗口在前台时按键转发给设备\n方向键走，Z 开火，空格开门，Esc 菜单")
        label.frame = NSRect(x: 16, y: 50, width: 390, height: 60)
        let st = NSTextField(labelWithString: "连接中…")
        st.frame = NSRect(x: 16, y: 16, width: 390, height: 24)
        v.addSubview(label); v.addSubview(st)
        link.status = { st.stringValue = $0 }
        window.contentView = v
        window.center()
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(v)
        NSApp.activate(ignoringOtherApps: true)
        link.start()
    }
    func applicationDidResignActive(_ n: Notification) { link.releaseAll() }
    func applicationShouldTerminateAfterLastWindowClosed(_ s: NSApplication) -> Bool { true }
}

let args = CommandLine.arguments
if args.contains("--selftest") { exit(selftest()) }
var iface = 7
if let i = args.firstIndex(of: "--interface"), i + 1 < args.count, let n = Int(args[i + 1]) { iface = n }
let app = NSApplication.shared
app.setActivationPolicy(.regular)
let delegate = AppDelegate(link: Link(iface: iface))
app.delegate = delegate
app.run()
