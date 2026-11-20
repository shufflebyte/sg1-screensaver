import CoreGraphics
import Foundation
import ImageIO
import Metal
import UniformTypeIdentifiers
import EgyptCore

/// Testwerkzeug: rendert einzelne Bilder mit demselben Renderer wie der Bildschirmschoner
/// in eine PNG-Datei und misst die Zeit pro Bild.
///   snapshot <ausgabe.png> [breite] [höhe] [sekunde] [palette 0|1]
@main
struct Snapshot {
    static func main() {
        let a = CommandLine.arguments
        guard a.count >= 2 else {
            print("Aufruf: snapshot <ausgabe.png> [breite] [höhe] [sekunde] [palette]")
            exit(1)
        }
        let w = a.count > 2 ? Int(a[2]) ?? 1920 : 1920
        let h = a.count > 3 ? Int(a[3]) ?? 1080 : 1080
        let t = a.count > 4 ? Double(a[4]) ?? 2 : 2
        guard let device = MTLCreateSystemDefaultDevice() else {
            fputs("Kein Metal-Gerät verfügbar\n", stderr)
            exit(2)
        }
        var s = EgyptSettings()
        egypt_default_settings(&s)
        if a.count > 5, let p = Int32(a[5]) { s.palette = p }

        guard let r = EgyptRenderer(device: device, pixelFormat: .bgra8Unorm, settings: s) else {
            fputs("Renderer konnte nicht erstellt werden\n", stderr)
            exit(3)
        }
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = device.hasUnifiedMemory ? .shared : .managed
        let tex = device.makeTexture(descriptor: d)!

        func frame(_ time: Double) {
            let cb = r.makeCommandBuffer()!
            r.encode(cb, target: tex, time: time, scale: 1)
            if d.storageMode == .managed, let blit = cb.makeBlitCommandEncoder() {
                blit.synchronize(resource: tex)
                blit.endEncoding()
            }
            cb.commit()
            cb.waitUntilCompleted()
        }

        let t0 = CFAbsoluteTimeGetCurrent()
        frame(t)  // erstes Bild baut auch die Wand-Textur
        let t1 = CFAbsoluteTimeGetCurrent()
        let n = 60
        for i in 0..<n { frame(t + Double(i) / 60) }
        let t2 = CFAbsoluteTimeGetCurrent()
        frame(t)
        print(String(format: "%dx%d: erstes Bild %.0f ms, danach %.2f ms pro Bild (Gerät: %@)",
                     w, h, (t1 - t0) * 1000, (t2 - t1) * 1000 / Double(n), device.name))

        var bytes = [UInt8](repeating: 0, count: w * h * 4)
        tex.getBytes(&bytes, bytesPerRow: w * 4, from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
        let info = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
        let image: CGImage? = bytes.withUnsafeMutableBytes { raw in
            CGContext(data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                      space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: info)?.makeImage()
        }
        guard let img = image,
              let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: a[1]) as CFURL,
                                                         UTType.png.identifier as CFString, 1, nil) else {
            fputs("PNG konnte nicht geschrieben werden\n", stderr)
            exit(4)
        }
        CGImageDestinationAddImage(dest, img, nil)
        CGImageDestinationFinalize(dest)
    }
}
