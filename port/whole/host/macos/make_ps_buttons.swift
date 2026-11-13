// Draws the PlayStation-style button icons that replace the game's GameCube ones (texture pack in Dolphin naming).
// Usage: swift make_ps_buttons.swift <buttons.json> <out dir> [scale]
// buttons.json: [{label, name, w, h, box: [x0, y0, x1, y1]}] (tools: the names come from the disc's core.das).
// Labels: A -> cross, B -> circle, X -> square, Y -> triangle, R -> R2, L -> L2, START -> OPTIONS.
import AppKit
import Foundation

struct Button: Decodable { let label: String; let name: String; let w: Int; let h: Int; let box: [Int] }

let args = CommandLine.arguments
let buttons = try! JSONDecoder().decode([Button].self, from: Data(contentsOf: URL(fileURLWithPath: args[1])))
let outDir = args[2]
let scale = args.count > 3 ? CGFloat(Double(args[3])!) : 4
try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)

func rgb(_ hex: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(red: CGFloat((hex >> 16) & 255) / 255, green: CGFloat((hex >> 8) & 255) / 255, blue: CGFloat(hex & 255) / 255, alpha: a)
}

// a round face button: dark body, soft rim, the symbol in its colour
func faceButton(_ ctx: CGContext, _ r: CGRect, symbol: String) {
    let d = min(r.width, r.height)
    let c = CGRect(x: r.midX - d / 2, y: r.midY - d / 2, width: d, height: d).insetBy(dx: d * 0.03, dy: d * 0.03)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -d * 0.03), blur: d * 0.06, color: rgb(0x000000, 0.7))
    ctx.addEllipse(in: c); ctx.setFillColor(rgb(0x18181a)); ctx.fillPath()
    ctx.restoreGState()
    ctx.saveGState()
    ctx.addEllipse(in: c); ctx.clip()
    let g = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: [rgb(0x46464a), rgb(0x1c1c1f)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(g, start: CGPoint(x: c.midX, y: c.maxY), end: CGPoint(x: c.midX, y: c.minY), options: [])
    ctx.restoreGState()
    ctx.setStrokeColor(rgb(0xd8d8dc, 0.85)); ctx.setLineWidth(d * 0.045)
    ctx.strokeEllipse(in: c.insetBy(dx: d * 0.025, dy: d * 0.025))
    let s = d * 0.27, lw = d * 0.085
    let m = CGPoint(x: c.midX, y: c.midY)
    ctx.setLineWidth(lw); ctx.setLineCap(.round); ctx.setLineJoin(.round)
    switch symbol {
    case "cross":
        ctx.setStrokeColor(rgb(0x7fb6f5))
        ctx.move(to: CGPoint(x: m.x - s, y: m.y - s)); ctx.addLine(to: CGPoint(x: m.x + s, y: m.y + s))
        ctx.move(to: CGPoint(x: m.x - s, y: m.y + s)); ctx.addLine(to: CGPoint(x: m.x + s, y: m.y - s))
        ctx.strokePath()
    case "circle":
        ctx.setStrokeColor(rgb(0xf26b6b))
        ctx.strokeEllipse(in: CGRect(x: m.x - s * 1.05, y: m.y - s * 1.05, width: s * 2.1, height: s * 2.1))
    case "square":
        ctx.setStrokeColor(rgb(0xe896d4))
        ctx.stroke(CGRect(x: m.x - s * 0.95, y: m.y - s * 0.95, width: s * 1.9, height: s * 1.9))
    default:  // triangle
        ctx.setStrokeColor(rgb(0x44d9a6))
        let t = s * 1.15
        ctx.move(to: CGPoint(x: m.x, y: m.y + t)); ctx.addLine(to: CGPoint(x: m.x + t * 0.98, y: m.y - t * 0.66))
        ctx.addLine(to: CGPoint(x: m.x - t * 0.98, y: m.y - t * 0.66)); ctx.closePath(); ctx.strokePath()
    }
}

func text(_ ctx: CGContext, _ s: String, in r: CGRect, size: CGFloat, color: NSColor) {
    let font = NSFont(name: "Helvetica Neue Bold", size: size) ?? NSFont.boldSystemFont(ofSize: size)
    let a = NSAttributedString(string: s, attributes: [.font: font, .foregroundColor: color])
    let sz = a.size()
    a.draw(at: NSPoint(x: r.midX - sz.width / 2, y: r.midY - sz.height / 2 + size * 0.05))
}

// a trigger (L2 / R2): the rounded wedge of the pad's triggers, label in white
func trigger(_ ctx: CGContext, _ r: CGRect, label: String) {
    let w = r.width, h = r.height
    let body = CGMutablePath()
    let rr = r.insetBy(dx: w * 0.04, dy: h * 0.06)
    body.move(to: CGPoint(x: rr.minX + rr.width * 0.08, y: rr.minY))
    body.addLine(to: CGPoint(x: rr.maxX - rr.width * 0.08, y: rr.minY))
    body.addQuadCurve(to: CGPoint(x: rr.maxX, y: rr.minY + rr.height * 0.25), control: CGPoint(x: rr.maxX, y: rr.minY))
    body.addLine(to: CGPoint(x: rr.maxX - rr.width * 0.06, y: rr.maxY - rr.height * 0.3))
    body.addQuadCurve(to: CGPoint(x: rr.midX, y: rr.maxY), control: CGPoint(x: rr.maxX - rr.width * 0.12, y: rr.maxY))
    body.addQuadCurve(to: CGPoint(x: rr.minX + rr.width * 0.06, y: rr.maxY - rr.height * 0.3), control: CGPoint(x: rr.minX + rr.width * 0.12, y: rr.maxY))
    body.addLine(to: CGPoint(x: rr.minX, y: rr.minY + rr.height * 0.25))
    body.addQuadCurve(to: CGPoint(x: rr.minX + rr.width * 0.08, y: rr.minY), control: CGPoint(x: rr.minX, y: rr.minY))
    body.closeSubpath()
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -h * 0.03), blur: h * 0.06, color: rgb(0x000000, 0.7))
    ctx.addPath(body); ctx.setFillColor(rgb(0x1c1c1f)); ctx.fillPath()
    ctx.restoreGState()
    ctx.saveGState(); ctx.addPath(body); ctx.clip()
    let g = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: [rgb(0x4a4a4e), rgb(0x1c1c1f)] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(g, start: CGPoint(x: r.midX, y: r.maxY), end: CGPoint(x: r.midX, y: r.minY), options: [])
    ctx.restoreGState()
    ctx.addPath(body); ctx.setStrokeColor(rgb(0xd8d8dc, 0.85)); ctx.setLineWidth(min(w, h) * 0.05); ctx.strokePath()
    text(ctx, label, in: rr.offsetBy(dx: 0, dy: -rr.height * 0.05), size: rr.height * 0.48, color: .white)
}

// OPTIONS: a pill with the word
func pill(_ ctx: CGContext, _ r: CGRect, label: String) {
    let p = CGPath(roundedRect: r.insetBy(dx: r.height * 0.06, dy: r.height * 0.12), cornerWidth: r.height * 0.38, cornerHeight: r.height * 0.38, transform: nil)
    ctx.addPath(p); ctx.setFillColor(rgb(0x1c1c1f, 0.92)); ctx.fillPath()
    ctx.addPath(p); ctx.setStrokeColor(rgb(0xd8d8dc, 0.85)); ctx.setLineWidth(r.height * 0.06); ctx.strokePath()
    text(ctx, label, in: r, size: r.height * 0.33, color: .white)
}

for b in buttons {
    let W = Int(CGFloat(b.w) * scale), H = Int(CGFloat(b.h) * scale)
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: W, pixelsHigh: H, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let ctx = NSGraphicsContext.current!.cgContext
    ctx.clear(CGRect(x: 0, y: 0, width: W, height: H))
    // the box the game's icon covers (texel rows from the top; CoreGraphics counts from the bottom)
    let box = CGRect(x: CGFloat(b.box[0]) * scale, y: CGFloat(b.h - b.box[3]) * scale,
                     width: CGFloat(b.box[2] - b.box[0]) * scale, height: CGFloat(b.box[3] - b.box[1]) * scale)
    switch b.label {
    case "A": faceButton(ctx, box, symbol: "cross")
    case "B": faceButton(ctx, box, symbol: "circle")
    case "X": faceButton(ctx, box, symbol: "square")
    case "Y": faceButton(ctx, box, symbol: "triangle")
    case "R": trigger(ctx, box, label: "R2")
    case "L": trigger(ctx, box, label: "L2")
    case "START": pill(ctx, box, label: "OPTIONS")
    default: NSGraphicsContext.restoreGraphicsState(); continue
    }
    NSGraphicsContext.restoreGraphicsState()
    // any palette: Dolphin's "$" in place of the palette hash
    var parts = b.name.split(separator: "_").map(String.init)
    if parts.count == 5 { parts[3] = "$" }
    let file = "\(outDir)/\(parts.joined(separator: "_")).png"
    try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: file))
    print(b.label, "->", (file as NSString).lastPathComponent)
}
