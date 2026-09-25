import AppKit

let size = NSSize(width: 1024, height: 1024)
let image = NSImage(size: size)
image.lockFocus()

NSColor(calibratedRed: 0.09, green: 0.10, blue: 0.12, alpha: 1).setFill()
NSBezierPath(roundedRect: NSRect(origin: .zero, size: size), xRadius: 220, yRadius: 220).fill()
NSColor(calibratedRed: 0.14, green: 0.15, blue: 0.18, alpha: 1).setFill()
NSBezierPath(roundedRect: NSRect(x: 104, y: 110, width: 816, height: 804), xRadius: 110, yRadius: 110).fill()

NSColor(calibratedRed: 0.36, green: 0.39, blue: 1, alpha: 1).setStroke()
let border = NSBezierPath(roundedRect: NSRect(x: 104, y: 110, width: 816, height: 804), xRadius: 110, yRadius: 110)
border.lineWidth = 24
border.stroke()

let text = "NS6" as NSString
let attributes: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 255, weight: .heavy), .foregroundColor: NSColor.white]
let textSize = text.size(withAttributes: attributes)
text.draw(at: NSPoint(x: (1024 - textSize.width) / 2, y: 468 - textSize.height / 2), withAttributes: attributes)

for (index, color) in [NSColor.systemGreen, NSColor.systemIndigo, NSColor.systemOrange, NSColor.systemRed, NSColor.systemGreen].enumerated() {
    color.setFill()
    NSBezierPath(ovalIn: NSRect(x: 221 + index * 127, y: 725, width: 74, height: 74)).fill()
}
image.unlockFocus()

let representation = NSBitmapImageRep(data: image.tiffRepresentation!)!
try representation.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
