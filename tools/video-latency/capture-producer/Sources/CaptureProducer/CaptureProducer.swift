import CoreGraphics
import CoreImage
import CoreMedia
import Darwin
import Foundation
import AppKit
import ScreenCaptureKit

private enum Exit: Int32 {
    case usage = 2
    case permissionDenied = 3
    case captureFailed = 5
    case ocrFailure = 6
}

private struct ProducerError: Error, CustomStringConvertible {
    let description: String
}

private struct Arguments {
    var mode = "capture"
    var application = "Windows App"
    var titleContains: String?
    var windowID: CGWindowID?
    var outputDirectory: URL?
    var imageURL: URL?
    var syntheticTimestamp = "1750000000123"
    var syntheticFrameID = "481"
    var count = 1_000
    var intervalMilliseconds = 0
    var showsCursor = false

    static let usage = """
    Usage:
      capture-producer --preflight
      capture-producer --list-windows
      capture-producer --ocr-png FILE
      capture-producer --make-synthetic-png FILE [--synthetic-timestamp N] [--synthetic-frame-id N]
      capture-producer [--application NAME] [--window-title-contains TEXT | --window-id ID]
        --output-dir DIR [--count N] [--interval-ms N] [--shows-cursor]

    Formal capture uses Vision OCR from each exact captured PNG. --count defaults to
    1000 and cannot be lower; failed OCR is preserved in its sidecar and not emitted.
    """

    static func parse(_ values: [String]) throws -> Arguments {
        var result = Arguments()
        var index = 0
        func value(after option: String) throws -> String {
            guard index + 1 < values.count else {
                throw ProducerError(description: "missing value after \(option)")
            }
            index += 1
            return values[index]
        }

        while index < values.count {
            let option = values[index]
            switch option {
            case "--help", "-h":
                result.mode = "help"
            case "--preflight":
                result.mode = "preflight"
            case "--list-windows":
                result.mode = "list"
            case "--ocr-png":
                result.mode = "ocr"
                result.imageURL = URL(fileURLWithPath: try value(after: option))
            case "--make-synthetic-png":
                result.mode = "synthetic"
                result.imageURL = URL(fileURLWithPath: try value(after: option))
            case "--synthetic-timestamp":
                result.syntheticTimestamp = try value(after: option)
            case "--synthetic-frame-id":
                result.syntheticFrameID = try value(after: option)
            case "--application":
                result.application = try value(after: option)
            case "--window-title-contains":
                result.titleContains = try value(after: option)
            case "--window-id":
                let raw = try value(after: option)
                guard let parsed = CGWindowID(raw) else {
                    throw ProducerError(description: "invalid --window-id: \(raw)")
                }
                result.windowID = parsed
            case "--output-dir":
                result.outputDirectory = URL(fileURLWithPath: try value(after: option))
            case "--count":
                let raw = try value(after: option)
                guard let parsed = Int(raw), parsed >= 1_000 else {
                    throw ProducerError(description: "--count must be at least 1000 for a formal run")
                }
                result.count = parsed
            case "--interval-ms":
                let raw = try value(after: option)
                guard let parsed = Int(raw), parsed >= 0 else {
                    throw ProducerError(description: "--interval-ms must be a nonnegative integer")
                }
                result.intervalMilliseconds = parsed
            case "--shows-cursor":
                result.showsCursor = true
            default:
                throw ProducerError(description: "unknown argument: \(option)")
            }
            index += 1
        }

        if result.mode == "capture" {
            guard result.outputDirectory != nil else {
                throw ProducerError(description: "--output-dir is required")
            }
            if result.windowID != nil && result.titleContains != nil {
                throw ProducerError(description: "choose --window-id or --window-title-contains, not both")
            }
        } else if (result.mode == "ocr" || result.mode == "synthetic") && result.imageURL == nil {
            throw ProducerError(description: "image path is required")
        }
        return result
    }
}

private final class MonotonicClock: @unchecked Sendable {
    private let timebase: mach_timebase_info_data_t

    init() {
        var value = mach_timebase_info_data_t()
        mach_timebase_info(&value)
        timebase = value
    }

    func nanoseconds() -> UInt64 {
        let ticks = mach_absolute_time()
        let denominator = UInt64(timebase.denom)
        let numerator = UInt64(timebase.numer)
        return (ticks / denominator) * numerator + (ticks % denominator) * numerator / denominator
    }
}

private func writeJSON(_ value: Any, to handle: FileHandle = .standardOutput) throws {
    let data = try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
    handle.write(data)
    handle.write(Data([0x0a]))
}

private func preflightRecord() -> [String: Any] {
    [
        "capture_api": "ScreenCaptureKit.SCScreenshotManager.captureSampleBuffer",
        "local_monotonic_ns": MonotonicClock().nanoseconds(),
        "permission_api": "CGPreflightScreenCaptureAccess",
        "permission_instructions": "Open System Settings > Privacy & Security > Screen Recording, enable the app that launches capture-producer (Terminal, iTerm, or Codex), then quit and reopen that app if macOS requests it. Do not enable Windows App.",
        "permission_request_performed": false,
        "screen_recording_authorized": CGPreflightScreenCaptureAccess(),
        "target": "capture-producer executable capturing a Windows App process/window via ScreenCaptureKit",
        "timestamp_clock": "mach_absolute_time converted with mach_timebase_info",
        "timestamp_compatible_with": "cross-process Python time.monotonic_ns() on macOS; collector must use Python 3.10+",
    ]
}

private func shareableWindows() async throws -> [SCWindow] {
    let content = try await SCShareableContent.excludingDesktopWindows(
        true,
        onScreenWindowsOnly: true
    )
    return content.windows
}

private func windowRecord(_ window: SCWindow) -> [String: Any] {
    [
        "application": window.owningApplication?.applicationName ?? NSNull(),
        "bundle_id": window.owningApplication?.bundleIdentifier ?? NSNull(),
        "frame": [
            "height": window.frame.height,
            "width": window.frame.width,
            "x": window.frame.origin.x,
            "y": window.frame.origin.y,
        ],
        "on_screen": window.isOnScreen,
        "title": window.title ?? NSNull(),
        "window_id": window.windowID,
    ]
}

private func selectWindow(_ windows: [SCWindow], arguments: Arguments) throws -> SCWindow {
    let matches = windows.filter { window in
        if let windowID = arguments.windowID {
            return window.windowID == windowID
        }
        guard window.owningApplication?.applicationName == arguments.application else {
            return false
        }
        guard let title = arguments.titleContains else { return true }
        return window.title?.localizedCaseInsensitiveContains(title) == true
    }
    guard matches.count == 1 else {
        let candidates = matches.map { "\($0.windowID): \($0.title ?? "<untitled>")" }.joined(separator: ", ")
        throw ProducerError(
            description: "window selection matched \(matches.count) windows; use --window-id. Matches: \(candidates)"
        )
    }
    return matches[0]
}

private func captureImage(sampleBuffer: CMSampleBuffer) throws -> CGImage {
    guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
        throw ProducerError(description: "ScreenCaptureKit returned no image buffer")
    }
    let image = CIImage(cvPixelBuffer: pixelBuffer)
    let context = CIContext(options: [.cacheIntermediates: false])
    guard let cgImage = context.createCGImage(image, from: image.extent) else {
        throw ProducerError(description: "could not convert capture buffer to CGImage")
    }
    return cgImage
}

@main
private struct CaptureProducer {
    static func main() async {
        do {
            let arguments = try Arguments.parse(Array(CommandLine.arguments.dropFirst()))
            if arguments.mode == "help" {
                print(Arguments.usage)
                return
            }
            if arguments.mode == "synthetic" {
                guard let imageURL = arguments.imageURL else { return }
                let image = try PatternOCR.syntheticImage(
                    timestamp: arguments.syntheticTimestamp,
                    frameID: arguments.syntheticFrameID
                )
                try PatternOCR.writePNG(image, to: imageURL)
                return
            }
            if arguments.mode == "ocr" {
                guard let imageURL = arguments.imageURL else { return }
                let image = try PatternOCR.loadImage(imageURL)
                let result = try PatternOCR.recognize(image)
                var record = result.rawRecord
                record["status"] = result.metadata == nil ? "ocr_parse_failed" : "ok"
                record["failure_reason"] = result.failureReason ?? NSNull()
                if let metadata = result.metadata {
                    record["source_timestamp_server_ms"] = metadata.sourceTimestampServerMS
                    record["frame_id"] = metadata.frameID
                }
                try writeJSON(record)
                if result.metadata == nil { Darwin.exit(Exit.ocrFailure.rawValue) }
                return
            }

            let authorized = CGPreflightScreenCaptureAccess()
            if arguments.mode == "preflight" {
                try writeJSON(preflightRecord())
                if !authorized { Darwin.exit(Exit.permissionDenied.rawValue) }
                return
            }
            guard authorized else {
                try writeJSON(preflightRecord(), to: .standardError)
                throw ProducerError(
                    description: "Screen Recording permission is absent; no permission request was made"
                )
            }

            let windows = try await shareableWindows()
            if arguments.mode == "list" {
                try writeJSON(["windows": windows.map(windowRecord)])
                return
            }

            let hadOCRFailures = try await capture(arguments: arguments, windows: windows)
            if hadOCRFailures { Darwin.exit(Exit.ocrFailure.rawValue) }
        } catch let error as ProducerError {
            FileHandle.standardError.write(Data("capture-producer: \(error.description)\n".utf8))
            if error.description.contains("permission is absent") {
                Darwin.exit(Exit.permissionDenied.rawValue)
            }
            Darwin.exit(Exit.usage.rawValue)
        } catch {
            FileHandle.standardError.write(Data("capture-producer: \(error)\n".utf8))
            Darwin.exit(Exit.captureFailed.rawValue)
        }
    }

    private static func capture(arguments: Arguments, windows: [SCWindow]) async throws -> Bool {
        let window = try selectWindow(windows, arguments: arguments)
        guard let outputDirectory = arguments.outputDirectory else { return false }
        try FileManager.default.createDirectory(
            at: outputDirectory,
            withIntermediateDirectories: true
        )
        // SCContentFilter(desktopIndependentWindow:) reaches CoreGraphics/SLS.
        // A pure SwiftPM CLI has no NSApplication bootstrap, so initialize the
        // AppKit/CGS client before constructing the filter. Prohibited policy
        // keeps this capture helper from creating or activating a user-facing app.
        await MainActor.run {
            _ = NSApplication.shared
            NSApp.setActivationPolicy(.prohibited)
        }
        let clock = MonotonicClock()
        let filter = SCContentFilter(desktopIndependentWindow: window)
        let configuration = SCStreamConfiguration()
        configuration.width = max(1, Int(filter.contentRect.width * CGFloat(filter.pointPixelScale)))
        configuration.height = max(1, Int(filter.contentRect.height * CGFloat(filter.pointPixelScale)))
        configuration.pixelFormat = kCVPixelFormatType_32BGRA
        configuration.showsCursor = arguments.showsCursor
        configuration.ignoreShadowsSingleWindow = true

        let sessionStarted = clock.nanoseconds()
        var frameSidecars: [String] = []
        var emittedCount = 0
        var ocrFailureCount = 0
        for index in 0..<arguments.count {
            if index > 0 && arguments.intervalMilliseconds > 0 {
                try await Task.sleep(for: .milliseconds(arguments.intervalMilliseconds))
            }
            let before = clock.nanoseconds()
            let sampleBuffer = try await SCScreenshotManager.captureSampleBuffer(
                contentFilter: filter,
                configuration: configuration
            )
            let after = clock.nanoseconds()

            let stem = String(format: "frame-%06d", index)
            let pngURL = outputDirectory.appendingPathComponent(stem + ".png").standardizedFileURL
            let sidecarURL = outputDirectory.appendingPathComponent(stem + ".metadata.json").standardizedFileURL
            let image = try captureImage(sampleBuffer: sampleBuffer)
            try PatternOCR.writePNG(image, to: pngURL)
            let presentation = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
            var presentationRecord: [String: Any] = [
                "timescale": presentation.timescale,
                "valid": presentation.isValid,
                "value": presentation.value,
            ]
            if presentation.seconds.isFinite {
                presentationRecord["seconds"] = presentation.seconds
            }
            var sidecar: [String: Any] = [
                "attempt_index": index,
                "capture_after_monotonic_ns": after,
                "capture_api": "ScreenCaptureKit.SCScreenshotManager.captureSampleBuffer",
                "capture_before_monotonic_ns": before,
                "capture_interval_semantics": "before API invocation through completion callback return",
                "height_pixels": image.height,
                "metadata_mode": "vision",
                "png": pngURL.path,
                "sck_presentation_timestamp": presentationRecord,
                "width_pixels": image.width,
                "window": windowRecord(window),
            ]

            var sourceTimestamp: Double?
            var frameID: String?
            let timestampCropURL = outputDirectory
                .appendingPathComponent(stem + ".timestamp-crop.png")
                .standardizedFileURL
            let frameCropURL = outputDirectory
                .appendingPathComponent(stem + ".frame-crop.png")
                .standardizedFileURL
            do {
                try PatternOCR.writePNG(
                    try PatternOCR.crop(image, region: PatternOCR.timestampRegion),
                    to: timestampCropURL
                )
                try PatternOCR.writePNG(
                    try PatternOCR.crop(image, region: PatternOCR.frameRegion),
                    to: frameCropURL
                )
                let result = try PatternOCR.recognize(image)
                sidecar["ocr"] = result.rawRecord
                sidecar["ocr_crops"] = [
                    "frame": frameCropURL.path,
                    "timestamp": timestampCropURL.path,
                ]
                sidecar["source_metadata_semantics"] = "Vision OCR of this exact captured image; exact regex parsing with no digit substitution or metadata reuse"
                if let metadata = result.metadata {
                    sourceTimestamp = metadata.sourceTimestampServerMS
                    frameID = metadata.frameID
                } else {
                    sidecar["failure_reason"] = result.failureReason ?? "unknown OCR parse failure"
                }
            } catch {
                sidecar["failure_reason"] = "Vision OCR error: \(error)"
                sidecar["ocr"] = [
                    "engine": "Vision.framework VNRecognizeTextRequest",
                    "raw_observations": [],
                ]
            }

            if let sourceTimestamp, let frameID {
                sidecar["status"] = "emitted"
                sidecar["frame_id"] = frameID
                sidecar["source_timestamp_server_ms"] = sourceTimestamp
            } else {
                sidecar["status"] = "ocr_parse_failed_not_emitted"
                ocrFailureCount += 1
            }
            let sidecarData = try JSONSerialization.data(
                withJSONObject: sidecar,
                options: [.prettyPrinted, .sortedKeys]
            )
            try sidecarData.write(to: sidecarURL, options: .withoutOverwriting)
            frameSidecars.append(sidecarURL.path)

            guard let sourceTimestamp, let frameID else { continue }
            try writeJSON([
                "capture_after_monotonic_ns": after,
                "capture_artifact": pngURL.path,
                "capture_before_monotonic_ns": before,
                "frame_id": frameID,
                "source_timestamp_server_ms": sourceTimestamp,
            ])
            emittedCount += 1
        }

        let allSidecarsPresent = frameSidecars.allSatisfy {
            FileManager.default.fileExists(atPath: $0)
        }
        let attemptsReconciled = frameSidecars.count == arguments.count
            && emittedCount + ocrFailureCount == arguments.count
            && allSidecarsPresent
        guard attemptsReconciled else {
            throw ProducerError(description: "capture attempts did not reconcile; refusing a partial session")
        }
        let session: [String: Any] = [
            "attempts_reconciled_without_omissions": attemptsReconciled,
            "calibration": [
                "performed": false,
                "reason": "Windows/source to server calibration belongs to collector.py and must bracket the live run",
            ],
            "capture_attempt_count": arguments.count,
            "frame_sidecars": frameSidecars,
            "jsonl_emitted_count": emittedCount,
            "metadata_mode": "vision",
            "ocr_failure_count": ocrFailureCount,
            "permission_request_performed": false,
            "producer_end_monotonic_ns": clock.nanoseconds(),
            "producer_start_monotonic_ns": sessionStarted,
            "screen_recording_authorized_at_start": true,
            "timestamp_clock": "mach_absolute_time converted with mach_timebase_info; collector must use cross-process Python 3.10+ time.monotonic_ns() on macOS",
            "window": windowRecord(window),
        ]
        let sessionURL = outputDirectory.appendingPathComponent("session.json")
        let sessionData = try JSONSerialization.data(
            withJSONObject: session,
            options: [.prettyPrinted, .sortedKeys]
        )
        try sessionData.write(to: sessionURL, options: .withoutOverwriting)
        return ocrFailureCount > 0
    }
}
