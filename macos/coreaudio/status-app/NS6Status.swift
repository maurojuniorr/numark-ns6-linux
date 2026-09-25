import AppKit
import CoreAudio
import IOKit

private let vendorID: UInt16 = 0x15e4
private let productID: UInt16 = 0x0079
private let driverVersion = "0.1.0"

private func usbConnected() -> Bool {
    var iterator: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &iterator) == KERN_SUCCESS else { return false }
    defer { IOObjectRelease(iterator) }
    while true {
        let service = IOIteratorNext(iterator)
        if service == 0 { return false }
        defer { IOObjectRelease(service) }
        let vendor = IORegistryEntryCreateCFProperty(service, "idVendor" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? NSNumber
        let product = IORegistryEntryCreateCFProperty(service, "idProduct" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? NSNumber
        if vendor?.uint16Value == vendorID && product?.uint16Value == productID { return true }
    }
}

private func audioDriverVisible() -> Bool {
    var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var byteCount: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &byteCount) == noErr else { return false }
    let count = Int(byteCount) / MemoryLayout<AudioDeviceID>.size
    var devices = Array(repeating: AudioDeviceID(0), count: count)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &byteCount, &devices) == noErr else { return false }
    for device in devices {
        var nameAddress = AudioObjectPropertyAddress(mSelector: kAudioObjectPropertyName, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var name: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout.size(ofValue: name))
        if AudioObjectGetPropertyData(device, &nameAddress, 0, nil, &size, &name) == noErr,
           let value = name?.takeRetainedValue() as String?, value == "Numark NS6" { return true }
    }
    return false
}

final class StatusController: NSViewController {
    private let statusLabel = NSTextField(labelWithString: "")
    private let stateDot = NSView()
    private var valueLabels: [NSTextField] = []
    private var timer: Timer?

    override func loadView() {
        let root = NSView()
        root.wantsLayer = true
        root.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
        view = root

        let logo = NSTextField(labelWithString: "NUMARK")
        logo.font = .systemFont(ofSize: 48, weight: .black)
        logo.textColor = .labelColor
        logo.alignment = .center

        let title = NSTextField(labelWithString: "NS6 Audio Driver")
        title.font = .systemFont(ofSize: 15, weight: .semibold)
        title.textColor = .secondaryLabelColor
        title.alignment = .center

        let rows = [
            ("Device", "Numark NS6"),
            ("Inputs", "Not implemented"),
            ("Outputs", "4"),
            ("Clock Rate", "44.1 kHz"),
            ("Word Length", "24-bit packed"),
            ("Driver Version", driverVersion),
            ("Driver Developer", "Mauro Junior / community"),
            ("Firmware Version", "Not queried")
        ]
        let details = NSStackView()
        details.orientation = .vertical
        details.alignment = .leading
        details.spacing = 9
        for (name, value) in rows {
            let label = NSTextField(labelWithString: name)
            label.font = .systemFont(ofSize: 14, weight: .semibold)
            let valueLabel = NSTextField(labelWithString: value)
            valueLabel.font = .monospacedSystemFont(ofSize: 14, weight: .regular)
            valueLabel.alignment = .right
            valueLabels.append(valueLabel)
            let row = NSStackView(views: [label, valueLabel])
            row.orientation = .horizontal
            row.distribution = .fillEqually
            row.alignment = .centerY
            details.addArrangedSubview(row)
        }

        stateDot.wantsLayer = true
        stateDot.layer?.cornerRadius = 6
        stateDot.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([stateDot.widthAnchor.constraint(equalToConstant: 12), stateDot.heightAnchor.constraint(equalToConstant: 12)])
        statusLabel.font = .systemFont(ofSize: 14, weight: .bold)
        let state = NSStackView(views: [stateDot, statusLabel])
        state.orientation = .horizontal
        state.alignment = .centerY
        state.spacing = 8
        state.edgeInsets = NSEdgeInsets(top: 12, left: 14, bottom: 12, right: 14)
        state.wantsLayer = true
        state.layer?.cornerRadius = 8
        state.layer?.borderWidth = 1
        state.layer?.borderColor = NSColor.separatorColor.cgColor

        let close = NSButton(title: "Close", target: self, action: #selector(closeWindow))
        close.bezelStyle = .rounded

        let stack = NSStackView(views: [logo, title, details, close, state])
        stack.orientation = .vertical
        stack.alignment = .centerX
        stack.spacing = 16
        stack.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 28),
            stack.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -28),
            stack.topAnchor.constraint(equalTo: root.topAnchor, constant: 28),
            stack.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -24),
            details.widthAnchor.constraint(equalTo: stack.widthAnchor),
            state.widthAnchor.constraint(equalTo: stack.widthAnchor)
        ])
    }

    override func viewDidAppear() {
        super.viewDidAppear()
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.refresh() }
    }

    override func viewWillDisappear() { timer?.invalidate(); timer = nil; super.viewWillDisappear() }

    private func refresh() {
        let connected = usbConnected()
        let driverVisible = audioDriverVisible()
        let ready = connected && driverVisible
        statusLabel.stringValue = ready ? "CONNECTED — audio driver ready" : (connected ? "CONNECTED — waiting for audio driver" : "NO DEVICE")
        stateDot.layer?.backgroundColor = (ready ? NSColor.systemGreen : (connected ? NSColor.systemOrange : NSColor.systemRed)).cgColor
        valueLabels[0].stringValue = connected ? "Numark NS6 (USB)" : "—"
        valueLabels[6].stringValue = connected ? "Not queried" : "—"
    }

    @objc private func closeWindow() { view.window?.close() }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        let controller = StatusController()
        let window = NSWindow(contentViewController: controller)
        window.title = "Numark NS6 Status"
        window.setContentSize(NSSize(width: 410, height: 475))
        window.styleMask = [.titled, .closable, .miniaturizable]
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
