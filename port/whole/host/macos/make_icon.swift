// Draws the app icon (a generic one: dark tile, red glow, "RE4" set in a serif face) as a 1024 px PNG.
// Usage: swift make_icon.swift <out.png>
import AppKit

let size = 1024.0
let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon.png"
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size), pixelsHigh: Int(size), bitsPerSample: 8,
                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                           bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext

// the macOS icon grid: a rounded square inset 100 px
let tile = CGRect(x: 100, y: 100, width: 824, height: 824)
let path = CGPath(roundedRect: tile, cornerWidth: 185, cornerHeight: 185, transform: nil)
ctx.saveGState()
ctx.addPath(path); ctx.clip()
let space = CGColorSpaceCreateDeviceRGB()
let bg = CGGradient(colorsSpace: space, colors: [CGColor(red: 0.16, green: 0.04, blue: 0.03, alpha: 1),
                                                  CGColor(red: 0.03, green: 0.02, blue: 0.02, alpha: 1)] as CFArray,
                    locations: [0, 1])!
ctx.drawRadialGradient(bg, startCenter: CGPoint(x: 512, y: 560), startRadius: 0,
                       endCenter: CGPoint(x: 512, y: 512), endRadius: 600, options: [.drawsAfterEndLocation])
// a thin warm frame
ctx.setStrokeColor(CGColor(red: 0.62, green: 0.48, blue: 0.28, alpha: 0.55))
ctx.setLineWidth(10)
ctx.addPath(CGPath(roundedRect: tile.insetBy(dx: 26, dy: 26), cornerWidth: 160, cornerHeight: 160, transform: nil))
ctx.strokePath()
ctx.restoreGState()

func draw(_ text: String, font: NSFont, color: NSColor, y: CGFloat, kern: CGFloat, shadow: NSShadow? = nil) {
    var attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color, .kern: kern]
    if let s = shadow { attrs[.shadow] = s }
    let s = NSAttributedString(string: text, attributes: attrs)
    let w = s.size().width
    s.draw(at: NSPoint(x: (size - w) / 2 + kern / 2, y: y))
}
let serif = NSFont(name: "Times New Roman Bold", size: 330) ?? NSFont.boldSystemFont(ofSize: 330)
let glow = NSShadow(); glow.shadowColor = NSColor(red: 0.9, green: 0.1, blue: 0.05, alpha: 0.9); glow.shadowBlurRadius = 60
draw("RE", font: serif, color: NSColor(red: 0.93, green: 0.89, blue: 0.80, alpha: 1), y: 420, kern: 18, shadow: glow)
let four = NSFont(name: "Times New Roman Bold", size: 260) ?? NSFont.boldSystemFont(ofSize: 260)
draw("4", font: four, color: NSColor(red: 0.80, green: 0.10, blue: 0.07, alpha: 1), y: 170, kern: 0, shadow: glow)
let small = NSFont(name: "Helvetica Neue", size: 54) ?? NSFont.systemFont(ofSize: 54)
draw("macOS", font: small, color: NSColor(red: 0.75, green: 0.65, blue: 0.50, alpha: 0.9), y: 760, kern: 14)

NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
