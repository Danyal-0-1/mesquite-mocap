// =========================================================================
//  THE HUB LINK  --  Transport/HubLink.swift
//
//  WHY NETWORK.FRAMEWORK
//  The hub is a plain ws:// server on its own Wi-Fi, with no Internet and no
//  TLS. URLSession would put App Transport Security in the way of a
//  cleartext connection; Network.framework is not subject to ATS, lets the
//  app forbid the cellular interface (so iOS never tries to reach
//  192.168.4.1 over LTE while the hub's Wi-Fi "has no Internet"), and
//  exposes TCP_NODELAY and keepalive. It ships with iOS and macOS, so the
//  same file is exercised on a Mac by the end-to-end check (../CODE.md).
//
//  PROTOCOL: what the hub accepts (Dongle_Binary.ino handleWebSocketMessage,
//  and the JSON dongle's): a complete, unfragmented TEXT message. Every pose
//  is one send with isComplete = true, which Network.framework puts in one
//  final frame. The hub never answers; it only forwards.
//
//  RECOVERY: latest pose wins. A pose that cannot go now is dropped, never
//  queued: a queued pose arrives late and the page would draw the past as
//  the present. At most two sends are in flight; beyond that the link is
//  backed up and the frame is counted as skipped. Any failure tears the
//  connection down and reconnects with backoff (0.5 s doubling to 4 s).
//
//  CONCURRENCY: callbacks run on the link's own queue; `send` is called from
//  the AR frame queue. The small shared state sits behind one lock.
// =========================================================================
#if canImport(Network)
import Foundation
import Network

/// @unchecked Sendable: every mutable field is touched only under `lock` or
/// on `queue` (CONCURRENCY above), which the compiler cannot see for itself.
public final class HubLink: @unchecked Sendable {
    public enum State: Equatable, Sendable {
        case stopped
        case connecting
        case ready
        /// Not connected; the text says why (shown in the app).
        case waiting(String)
    }

    public struct Stats: Equatable, Sendable {
        public var sent = 0
        public var completed = 0
        public var skippedBusy = 0
        public var sendErrors = 0
        public var connects = 0
        public var bytes = 0
    }

    /// Called on the link's queue whenever the state changes.
    public var onState: ((State) -> Void)?

    private let queue = DispatchQueue(label: "mesquite.hublink")
    private let lock = NSLock()
    private var state = State.stopped
    private var stats = Stats()
    private var inFlight = 0
    private var connection: NWConnection?
    private var url: URL?
    private var generation = 0
    private var retryDelay = 0.5
    private let maxInFlight = 2

    public init() {}

    public var snapshot: (state: State, stats: Stats) {
        lock.lock()
        defer { lock.unlock() }
        return (state, stats)
    }

    public func start(url: URL) {
        queue.async {
            self.url = url
            self.retryDelay = 0.5
            self.connect()
        }
    }

    public func stop() {
        queue.async {
            self.url = nil
            self.teardown()
            self.setState(.stopped)
        }
    }

    /// Hand one message to the hub. Returns false when it was not taken
    /// (not connected, or backed up); the caller then tries a later frame.
    @discardableResult
    public func send(_ text: String) -> Bool {
        lock.lock()
        guard state == .ready, let conn = connection, inFlight < maxInFlight else {
            if state == .ready { stats.skippedBusy += 1 }
            lock.unlock()
            return false
        }
        inFlight += 1
        stats.sent += 1
        stats.bytes += text.utf8.count
        let gen = generation
        lock.unlock()

        let metadata = NWProtocolWebSocket.Metadata(opcode: .text)
        let context = NWConnection.ContentContext(identifier: "pose", metadata: [metadata])
        conn.send(content: Data(text.utf8), contentContext: context, isComplete: true,
                  completion: .contentProcessed { [weak self] error in
                      self?.finished(generation: gen, error: error)
                  })
        return true
    }

    // ---------------------------------------------------------------- private, on `queue`

    private func connect() {
        guard let url else { return }
        teardown()

        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true              // one pose, one segment, now: no Nagle batching
        tcp.connectionTimeout = 3
        tcp.enableKeepalive = true      // notice a hub that lost power within ~5 s
        tcp.keepaliveIdle = 2
        tcp.keepaliveInterval = 1
        tcp.keepaliveCount = 3

        let ws = NWProtocolWebSocket.Options()
        ws.autoReplyPing = true

        let params = NWParameters(tls: nil, tcp: tcp)
        params.defaultProtocolStack.applicationProtocols.insert(ws, at: 0)
        params.prohibitedInterfaceTypes = [.cellular]

        let conn = NWConnection(to: .url(url), using: params)
        lock.lock()
        generation += 1
        let gen = generation
        connection = conn
        inFlight = 0
        lock.unlock()

        conn.stateUpdateHandler = { [weak self] s in self?.handle(s, generation: gen) }
        setState(.connecting)
        conn.start(queue: queue)
    }

    private func handle(_ s: NWConnection.State, generation gen: Int) {
        guard gen == currentGeneration else { return }
        switch s {
        case .ready:
            retryDelay = 0.5
            lock.lock()
            stats.connects += 1
            lock.unlock()
            setState(.ready)
            receive(on: connection, generation: gen)
        case .waiting(let error):
            // .waiting retries only when the network changes; a hub that
            // refused or timed out is retried on a timer instead.
            setState(.waiting(Self.describe(error)))
            scheduleReconnect()
        case .failed(let error):
            setState(.waiting(Self.describe(error)))
            scheduleReconnect()
        case .setup, .preparing:
            setState(.connecting)
        case .cancelled:
            break
        @unknown default:
            break
        }
    }

    /// The hub sends nothing, but reading lets Network.framework process a
    /// close or a ping (autoReplyPing answers it).
    private func receive(on conn: NWConnection?, generation gen: Int) {
        conn?.receiveMessage { [weak self] _, _, _, error in
            guard let self, gen == self.currentGeneration else { return }
            if error == nil { self.receive(on: conn, generation: gen) }
        }
    }

    private func finished(generation gen: Int, error: NWError?) {
        lock.lock()
        if gen == generation {
            inFlight = max(0, inFlight - 1)
            if error == nil { stats.completed += 1 } else { stats.sendErrors += 1 }
        }
        lock.unlock()
        if error != nil {
            queue.async {
                guard gen == self.currentGeneration else { return }
                self.setState(.waiting("send failed"))
                self.scheduleReconnect()
            }
        }
    }

    private func scheduleReconnect() {
        let gen = currentGeneration
        let delay = retryDelay
        retryDelay = min(4, retryDelay * 2)
        teardown()
        queue.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self, self.url != nil, gen == self.currentGeneration else { return }
            self.connect()
        }
    }

    private func teardown() {
        lock.lock()
        let old = connection
        connection = nil
        inFlight = 0
        lock.unlock()
        old?.stateUpdateHandler = nil
        old?.cancel()
    }

    private var currentGeneration: Int {
        lock.lock()
        defer { lock.unlock() }
        return generation
    }

    private func setState(_ s: State) {
        lock.lock()
        let changed = state != s
        state = s
        lock.unlock()
        if changed { onState?(s) }
    }

    private static func describe(_ error: NWError) -> String {
        switch error {
        case .posix(let code):
            switch code {
            case .ECONNREFUSED: return "hub refused the connection"
            case .ETIMEDOUT: return "hub did not answer (timed out)"
            case .ENETDOWN, .ENETUNREACH, .EHOSTUNREACH:
                return "no route to the hub: join its Wi-Fi"
            default: return "network error \(code.rawValue)"
            }
        case .dns: return "cannot resolve the hub's address"
        case .tls: return "TLS error (the hub uses plain ws://)"
        default: return "\(error)"
        }
    }
}
#endif
