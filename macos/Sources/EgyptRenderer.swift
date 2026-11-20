import Foundation
import Metal
import MetalKit
import QuartzCore
import EgyptCore

/// Zeichnet Wand, Figuren und Röhren-Look mit Metal.
/// Die Geometrie kommt aus dem C-Kern (core/egypt_core.c), hier wird nur gerendert.
final class EgyptRenderer: NSObject {
    static let sampleCount = 4

    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let bgPipe: MTLRenderPipelineState
    private let polyPipe: MTLRenderPipelineState
    private let crtPipe: MTLRenderPipelineState

    var settings: EgyptSettings { didSet { bgTexture = nil } }
    var startTime = CACurrentMediaTime()

    private var bgTexture: MTLTexture?
    private var msaaTexture: MTLTexture?
    private var buffers: [MTLBuffer?] = [nil, nil, nil]
    private var bufferIndex = 0
    private let inflight = DispatchSemaphore(value: 3)

    private struct Uniforms {
        var size: SIMD2<Float>
        var dpr: Float
        var pad: Float = 0
    }

    init?(device: MTLDevice, pixelFormat: MTLPixelFormat, settings: EgyptSettings) {
        guard let queue = device.makeCommandQueue() else { return nil }
        let library: MTLLibrary
        do {
            library = try device.makeLibrary(source: EgyptRenderer.shaderSource, options: nil)
        } catch {
            NSLog("EgyptSaver: Shader konnte nicht übersetzt werden: \(error)")
            return nil
        }
        func pipeline(_ vs: String, _ fs: String) -> MTLRenderPipelineState? {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = library.makeFunction(name: vs)
            d.fragmentFunction = library.makeFunction(name: fs)
            d.rasterSampleCount = EgyptRenderer.sampleCount
            let ca = d.colorAttachments[0]!
            ca.pixelFormat = pixelFormat
            ca.isBlendingEnabled = true
            ca.sourceRGBBlendFactor = .one
            ca.sourceAlphaBlendFactor = .one
            ca.destinationRGBBlendFactor = .oneMinusSourceAlpha
            ca.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            do { return try device.makeRenderPipelineState(descriptor: d) } catch {
                NSLog("EgyptSaver: Pipeline \(vs)/\(fs) fehlgeschlagen: \(error)")
                return nil
            }
        }
        guard let bg = pipeline("full_vs", "bg_fs"),
              let poly = pipeline("poly_vs", "poly_fs"),
              let crt = pipeline("full_vs", "crt_fs") else { return nil }

        self.device = device
        self.queue = queue
        self.bgPipe = bg
        self.polyPipe = poly
        self.crtPipe = crt
        self.settings = settings
        super.init()
    }

    func makeCommandBuffer() -> MTLCommandBuffer? { queue.makeCommandBuffer() }

    /// Rendert ein Bild in `target` (Größe = Zielgröße in Pixeln).
    func encode(_ cb: MTLCommandBuffer, target: MTLTexture, time: Double, scale dpr: Float) {
        let w = target.width, h = target.height
        guard w > 0, h > 0 else { return }
        prepareBackground(w, h)
        prepareMSAA(w, h, format: target.pixelFormat)
        guard let bg = bgTexture, let msaa = msaaTexture else { return }

        let W = Float(w), H = Float(h)
        var s = settings
        let cap = egypt_max_vertices(&s, W, H)
        let need = max(cap, 3) * MemoryLayout<EgyptVertex>.stride

        inflight.wait()
        let sem = inflight
        cb.addCompletedHandler { _ in sem.signal() }

        bufferIndex = (bufferIndex + 1) % buffers.count
        if (buffers[bufferIndex]?.length ?? 0) < need {
            buffers[bufferIndex] = device.makeBuffer(length: need, options: .storageModeShared)
        }
        guard let vb = buffers[bufferIndex] else { return }
        let ptr = vb.contents().bindMemory(to: EgyptVertex.self, capacity: max(cap, 3))
        let count = min(egypt_build_scene(&s, W, H, time, ptr, cap), cap)

        let rpd = MTLRenderPassDescriptor()
        rpd.colorAttachments[0].texture = msaa
        rpd.colorAttachments[0].resolveTexture = target
        rpd.colorAttachments[0].loadAction = .dontCare
        rpd.colorAttachments[0].storeAction = .multisampleResolve
        guard let enc = cb.makeRenderCommandEncoder(descriptor: rpd) else { return }

        var u = Uniforms(size: SIMD2(W, H), dpr: max(dpr, 1))

        enc.setRenderPipelineState(bgPipe)
        enc.setFragmentTexture(bg, index: 0)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)

        if count > 0 {
            enc.setRenderPipelineState(polyPipe)
            enc.setVertexBuffer(vb, offset: 0, index: 0)
            enc.setVertexBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: count)
        }

        if settings.crt != 0 {
            enc.setRenderPipelineState(crtPipe)
            enc.setFragmentBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        }
        enc.endEncoding()
    }

    private func prepareBackground(_ w: Int, _ h: Int) {
        if let t = bgTexture, t.width == w, t.height == h { return }
        var pixels = [UInt8](repeating: 0, count: w * h * 4)
        var s = settings
        egypt_build_background(&s, Int32(w), Int32(h), &pixels)
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: w, height: h, mipmapped: false)
        d.usage = .shaderRead
        guard let t = device.makeTexture(descriptor: d) else { return }
        pixels.withUnsafeBytes { raw in
            t.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0,
                      withBytes: raw.baseAddress!, bytesPerRow: w * 4)
        }
        bgTexture = t
    }

    private func prepareMSAA(_ w: Int, _ h: Int, format: MTLPixelFormat) {
        if let t = msaaTexture, t.width == w, t.height == h, t.pixelFormat == format { return }
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: w, height: h, mipmapped: false)
        d.textureType = .type2DMultisample
        d.sampleCount = EgyptRenderer.sampleCount
        d.usage = .renderTarget
        // Auf Apple-GPUs liegt das MSAA-Bild nur im Kachelspeicher (spart viel RAM bei 5K).
        d.storageMode = device.supportsFamily(.apple1) ? .memoryless : .private
        msaaTexture = device.makeTexture(descriptor: d)
    }

    static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct V    { packed_float2 pos; packed_float4 color; };   // = EgyptVertex (24 Byte)
    struct Uni  { float2 size; float dpr; float pad; };
    struct VOut { float4 position [[position]]; float4 color; };
    struct FOut { float4 position [[position]]; float2 uv; };

    vertex VOut poly_vs(const device V *v [[buffer(0)]], constant Uni &u [[buffer(1)]],
                        uint vid [[vertex_id]]) {
        float2 p = float2(v[vid].pos);
        VOut o;
        o.position = float4(p.x / u.size.x * 2.0 - 1.0, 1.0 - p.y / u.size.y * 2.0, 0.0, 1.0);
        o.color = float4(v[vid].color);
        return o;
    }

    fragment float4 poly_fs(VOut in [[stage_in]]) {
        return float4(in.color.rgb * in.color.a, in.color.a);
    }

    vertex FOut full_vs(uint vid [[vertex_id]]) {
        float2 p = float2((vid << 1) & 2, vid & 2);
        FOut o;
        o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
        o.uv = float2(p.x, 1.0 - p.y);
        return o;
    }

    fragment float4 bg_fs(FOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {
        constexpr sampler s(filter::linear, address::clamp_to_edge);
        return float4(tex.sample(s, in.uv).rgb, 1.0);
    }

    // Scanlines und abgedunkelte Ränder wie im Prototyp
    fragment float4 crt_fs(FOut in [[stage_in]], constant Uni &u [[buffer(0)]]) {
        float2 px = in.position.xy;
        float step = max(3.0, round(u.dpr * 3.0));
        float scan = fmod(px.y, step) < max(1.0, step / 3.0) ? 0.16 : 0.0;
        float r0 = min(u.size.x, u.size.y) * 0.35;
        float r1 = length(u.size) * 0.6;
        float vig = clamp((length(px - u.size * 0.5) - r0) / (r1 - r0), 0.0, 1.0) * 0.45;
        float a = 1.0 - (1.0 - scan) * (1.0 - vig);
        return float4(0.0, 0.0, 0.0, a);
    }
    """
}

extension EgyptRenderer: MTKViewDelegate {
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let drawable = view.currentDrawable, let cb = queue.makeCommandBuffer() else { return }
        let dpr = Float(view.drawableSize.width / max(view.bounds.width, 1))
        encode(cb, target: drawable.texture, time: CACurrentMediaTime() - startTime, scale: dpr)
        cb.present(drawable)
        cb.commit()
    }
}
