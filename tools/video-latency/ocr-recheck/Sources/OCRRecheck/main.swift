import CoreGraphics
import Foundation
import ImageIO
import Vision

private struct Candidate {
    let text: String
    let confidence: Float
}

private struct ResultRecord: Encodable {
    let attemptIndex: Int
    let image: String
    let originalStatus: String?
    let originalFailureReason: String?
    let timestampMatches: [String]
    let frameMatches: [String]
    let timestampCandidates: [String]
    let frameCandidates: [String]
    let status: String
    let failureReasons: [String]
}

private struct Summary: Encodable {
    let algorithm: String
    let timestampRegion: [String: Double]
    let frameRegion: [String: Double]
    let totalImages: Int
    let bothExact: Int
    let timestampOnly: Int
    let frameOnly: Int
    let neitherExact: Int
    let originalEmitted: Int
    let originalFailed: Int
    let recheckRecovered: Int
    let recheckLost: Int
}

private enum RecheckError: Error, CustomStringConvertible {
    case usage(String)
    case imageReadFailed(String)

    var description: String {
        switch self {
        case .usage(let text), .imageReadFailed(let text): return text
        }
    }
}

private let timestampRegion = CGRect(x: 0.0, y: 0.10, width: 0.52, height: 0.30)
private let frameRegion = CGRect(x: 0.0, y: 0.30, width: 0.25, height: 0.18)
private let algorithm = "vision-accurate-en-US-no-language-correction-fixed-narrow-roi-v1"

private func record(_ rect: CGRect) -> [String: Double] {
    ["x": rect.origin.x, "y": rect.origin.y, "width": rect.width, "height": rect.height]
}

private func loadImage(_ url: URL) throws -> CGImage {
    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
          let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
        throw RecheckError.imageReadFailed(url.path)
    }
    return image
}

private func candidates(in image: CGImage, region: CGRect) throws -> [Candidate] {
    let request = VNRecognizeTextRequest()
    request.recognitionLevel = .accurate
    request.recognitionLanguages = ["en-US"]
    request.usesLanguageCorrection = false
    request.regionOfInterest = region
    let handler = VNImageRequestHandler(cgImage: image, orientation: .up)
    try handler.perform([request])
    return (request.results ?? []).flatMap { observation in
        observation.topCandidates(3).map {
            Candidate(text: $0.string, confidence: $0.confidence)
        }
    }
}

private func matches(_ candidates: [Candidate], pattern: String) -> [String] {
    guard let regex = try? NSRegularExpression(pattern: pattern) else { return [] }
    var values = Set<String>()
    for candidate in candidates where candidate.confidence >= 0.5 {
        let range = NSRange(candidate.text.startIndex..., in: candidate.text)
        guard let match = regex.firstMatch(in: candidate.text, range: range),
              let valueRange = Range(match.range(at: 1), in: candidate.text) else { continue }
        values.insert(String(candidate.text[valueRange]))
    }
    return values.sorted()
}

private func jsonObject(_ url: URL) -> [String: Any]? {
    guard let data = try? Data(contentsOf: url),
          let value = try? JSONSerialization.jsonObject(with: data),
          let object = value as? [String: Any] else { return nil }
    return object
}

private func stringValue(_ object: [String: Any]?, _ key: String) -> String? {
    object?[key] as? String
}

private func attemptIndex(_ url: URL) -> Int {
    let name = url.deletingPathExtension().lastPathComponent
    return Int(name.replacingOccurrences(of: "frame-", with: "")) ?? -1
}

private func pngURLs(in directory: URL) throws -> [URL] {
    let urls = try FileManager.default.contentsOfDirectory(
        at: directory,
        includingPropertiesForKeys: nil
    )
    return urls.filter { url in
        url.pathExtension == "png" && !url.lastPathComponent.contains(".timestamp-crop") &&
        !url.lastPathComponent.contains(".frame-crop")
    }.sorted { attemptIndex($0) < attemptIndex($1) }
}

private func run(inputDirectory: URL, outputJSONL: URL, summaryURL: URL) throws {
    let urls = try pngURLs(in: inputDirectory)
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys]
    FileManager.default.createFile(atPath: outputJSONL.path, contents: nil)
    guard let output = FileHandle(forWritingAtPath: outputJSONL.path) else {
        throw RecheckError.usage("could not open output: \(outputJSONL.path)")
    }
    defer { try? output.close() }

    var bothExact = 0
    var timestampOnly = 0
    var frameOnly = 0
    var neitherExact = 0
    var originalEmitted = 0
    var originalFailed = 0
    var recovered = 0
    var lost = 0

    for url in urls {
        let sidecar = url.deletingPathExtension().appendingPathExtension("metadata.json")
        let original = jsonObject(sidecar)
        let originalStatus = stringValue(original, "status")
        let originalFailure = stringValue(original, "failure_reason")
        if originalStatus == "ok" { originalEmitted += 1 } else { originalFailed += 1 }

        let image = try loadImage(url)
        let timestamp = try candidates(in: image, region: timestampRegion)
        let frame = try candidates(in: image, region: frameRegion)
        let timestampValues = matches(timestamp, pattern: #"^\s*([0-9]{10,16})\s*$"#)
        let frameValues = matches(frame, pattern: #"(?i)^\s*FRAME[ \t]+([0-9]+)\s*$"#)
        let timestampOK = timestampValues.count == 1
        let frameOK = frameValues.count == 1
        if timestampOK && frameOK { bothExact += 1 }
        else if timestampOK { timestampOnly += 1 }
        else if frameOK { frameOnly += 1 }
        else { neitherExact += 1 }
        let status: String
        if timestampOK && frameOK { status = "ok" }
        else if timestampOK { status = "timestamp_only" }
        else if frameOK { status = "frame_only" }
        else { status = "failed" }
        if originalStatus == "ok" && status != "ok" { lost += 1 }
        if originalStatus != "ok" && status == "ok" { recovered += 1 }
        var failures: [String] = []
        if !timestampOK { failures.append("timestamp_exact_match_count_\(timestampValues.count)") }
        if !frameOK { failures.append("frame_exact_match_count_\(frameValues.count)") }
        let result = ResultRecord(
            attemptIndex: attemptIndex(url), image: url.lastPathComponent,
            originalStatus: originalStatus, originalFailureReason: originalFailure,
            timestampMatches: timestampValues, frameMatches: frameValues,
            timestampCandidates: timestamp.map(\.text), frameCandidates: frame.map(\.text),
            status: status, failureReasons: failures
        )
        output.write(try encoder.encode(result))
        output.write(Data([0x0a]))
    }

    let summary = Summary(
        algorithm: algorithm, timestampRegion: record(timestampRegion),
        frameRegion: record(frameRegion), totalImages: urls.count,
        bothExact: bothExact, timestampOnly: timestampOnly, frameOnly: frameOnly,
        neitherExact: neitherExact, originalEmitted: originalEmitted,
        originalFailed: originalFailed, recheckRecovered: recovered, recheckLost: lost
    )
    let summaryData = try encoder.encode(summary)
    try summaryData.write(to: summaryURL, options: .atomic)
}

do {
    let args = Array(CommandLine.arguments.dropFirst())
    guard args.count == 6, args[0] == "--input-dir", args[2] == "--output-jsonl",
          args[4] == "--summary-json" else {
        throw RecheckError.usage("usage: ocr-recheck --input-dir DIR --output-jsonl FILE --summary-json FILE")
    }
    try run(
        inputDirectory: URL(fileURLWithPath: args[1]),
        outputJSONL: URL(fileURLWithPath: args[3]),
        summaryURL: URL(fileURLWithPath: args[5])
    )
} catch {
    fputs("ocr-recheck: \(error)\n", stderr)
    exit(2)
}
