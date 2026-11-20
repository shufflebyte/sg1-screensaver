import AppKit
import MetalKit
import ScreenSaver
import EgyptCore

/// Bildschirmschoner-Ansicht. Der Name ist fest, weil Info.plist (NSPrincipalClass) darauf verweist.
@objc(EgyptSaverView)
final class EgyptSaverView: ScreenSaverView {
    private var mtkView: MTKView?
    private var renderer: EgyptRenderer?
    private var stopObserver: NSObjectProtocol?
    private var superviewObserver: NSObjectProtocol?
    private var windowObserver: NSObjectProtocol?

    override init?(frame: NSRect, isPreview: Bool) {
        super.init(frame: frame, isPreview: isPreview)
        setup()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setup()
    }

    deinit {
        if let o = stopObserver { DistributedNotificationCenter.default().removeObserver(o) }
        if let o = superviewObserver { NotificationCenter.default.removeObserver(o) }
        if let o = windowObserver { NotificationCenter.default.removeObserver(o) }
    }

    private func setup() {
        animationTimeInterval = 1.0 / 60.0
        // Der Host (legacyScreenSaver) hängt die Ansicht mit 0x0 in einen Container, der erst
        // später seine echte Größe bekommt. Ohne diese Maske bliebe die Ansicht 0x0.
        autoresizingMask = [.width, .height]
        translatesAutoresizingMaskIntoConstraints = true
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        makeMetalView()

        // Seit macOS 14 ruft das System stopAnimation nicht zuverlässig auf; alte Instanzen
        // würden unsichtbar weiterzeichnen. Darum beim Ende selbst anhalten.
        // Nicht mit exit(0) aussteigen: WallpaperAgent hält dann die Verbindung zum toten
        // Prozess fest und der nächste Start bleibt schwarz.
        if !isPreview {
            stopObserver = DistributedNotificationCenter.default().addObserver(
                forName: Notification.Name("com.apple.screensaver.willstop"), object: nil, queue: .main
            ) { [weak self] _ in self?.mtkView?.isPaused = true }
        }
    }

    private func makeMetalView() {
        guard mtkView == nil, let device = MTLCreateSystemDefaultDevice() else { return }
        let view = MTKView(frame: bounds, device: device)
        view.autoresizingMask = [.width, .height]
        view.colorPixelFormat = .bgra8Unorm
        view.sampleCount = 1                 // Kantenglättung macht der Renderer selbst
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        // Die Ansicht taktet sich selbst (Display-Link). Auf animateOneFrame ist
        // im Bildschirmschoner-Host neuerer macOS-Versionen kein Verlass.
        view.enableSetNeedsDisplay = false
        view.preferredFramesPerSecond = 60
        view.isPaused = false
        view.layer?.isOpaque = true
        (view.layer as? CAMetalLayer)?.colorspace = CGColorSpace(name: CGColorSpace.sRGB)

        var settings = EgyptSettings()
        egypt_default_settings(&settings)
        guard let r = EgyptRenderer(device: device, pixelFormat: view.colorPixelFormat, settings: settings) else { return }
        view.delegate = r
        addSubview(view)
        mtkView = view
        renderer = r
        updateScale()
    }

    /// macOS gibt alte Ansichten nicht frei. Damit sich pro Start kein Speicher ansammelt,
    /// Metal-Ansicht und Renderer beim Herausnehmen aus dem Fenster selbst wegwerfen.
    private func dropMetalView() {
        mtkView?.isPaused = true
        mtkView?.delegate = nil
        mtkView?.removeFromSuperview()
        mtkView = nil
        renderer = nil
    }

    /// Bildschärfe der Metal-Ebene an den Bildschirm anpassen. Wird die Ansicht in ein Fenster
    /// mit 0x0 gehängt (passiert zufällig bei etwa jedem zweiten Start), bleibt sie sonst auf 0
    /// und das gezeichnete Bild ist unsichtbar.
    private func updateScale() {
        guard let ml = mtkView?.layer else { return }
        let scale = window?.screen?.backingScaleFactor ?? window?.backingScaleFactor
            ?? NSScreen.main?.backingScaleFactor ?? 2
        if ml.contentsScale != scale { ml.contentsScale = scale }
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        updateScale()
    }

    /// Größe des Containers übernehmen und bei dessen Änderungen nachziehen.
    override func viewDidMoveToSuperview() {
        super.viewDidMoveToSuperview()
        if let o = superviewObserver { NotificationCenter.default.removeObserver(o); superviewObserver = nil }
        guard let sv = superview else { return }
        fitToSuperview()
        sv.postsFrameChangedNotifications = true
        superviewObserver = NotificationCenter.default.addObserver(
            forName: NSView.frameDidChangeNotification, object: sv, queue: .main
        ) { [weak self] _ in self?.fitToSuperview() }
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let o = windowObserver { NotificationCenter.default.removeObserver(o); windowObserver = nil }
        guard let w = window else {
            dropMetalView()
            return
        }
        makeMetalView()             // falls eine schon benutzte Ansicht wiederverwendet wird
        mtkView?.isPaused = false
        fitToSuperview()
        windowObserver = NotificationCenter.default.addObserver(
            forName: NSWindow.didResizeNotification, object: w, queue: .main
        ) { [weak self] _ in self?.fitToSuperview() }
        // Beim echten Start bekommt die Ansicht ihre volle Größe teils erst später und ohne
        // Benachrichtigung. Darum zusätzlich nachprüfen.
        for delay in [0.25, 1.0, 3.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                guard let self, self.window != nil else { return }
                self.fitToSuperview()
            }
        }
    }

    /// Normalerweise die Fläche des Containers. Ist der Container viel kleiner als der
    /// Fensterinhalt (der Host von macOS 27 liefert teils 112x16), den ganzen Fensterinhalt abdecken.
    private func fitToSuperview() {
        guard let sv = superview else { return }
        var target = sv.bounds
        if let content = window?.contentView, content !== sv {
            let c = sv.convert(content.bounds, from: content)
            if c.width > target.width * 1.5 || c.height > target.height * 1.5 { target = c }
        }
        if frame != target { frame = target }
        updateScale()
    }

    override func startAnimation() {
        super.startAnimation()
        renderer?.startTime = CACurrentMediaTime()
        mtkView?.isPaused = false
    }

    override func stopAnimation() {
        super.stopAnimation()
        mtkView?.isPaused = true
    }

    // Gezeichnet wird über den Display-Link der MTKView.
    override func animateOneFrame() {}

    override func draw(_ rect: NSRect) {
        NSColor.black.setFill()
        rect.fill()
    }

    override var hasConfigureSheet: Bool { false }
    override var configureSheet: NSWindow? { nil }
}
