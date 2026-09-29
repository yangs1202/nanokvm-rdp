import CoreGraphics
import CoreText
import Foundation
import ImageIO
import UniformTypeIdentifiers
import Vision

struct OCRParsedMetadata {
    let sourceTimestampServerMS: Double
    let frameID: String
}

struct OCRResult {
    let metadata: OCRParsedMetadata?
    let failureReason: String?
    let rawRecord: [String: Any]
}

enum PatternOCR {
    // Vision regions are normalized with the origin at the image's lower-left.
    // They intentionally overlap to tolerate browser/Windows App chrome and scaling.
    static let timestampRegion = CGRect(x: 0, y: 0.58, width: 1, height: 0.42)
    static let frameRegion = CGRect(x: 0, y: 0.40, width: 1, height: 0.40)
    static let minimumConfidence: Float = 0.5

    static func recognize(_ image: CGImage) throws -> OCRResult {
        let timestamp = try observations(in: image, region: timestampRegion)
        let frame = try observations(in: image, region: frameRegion)
        let timestampValues = uniqueMatches(
            timestamp,
            pattern: #"^\s*([0-9]{10,16})\s*$"#
        )
        let frameValues = uniqueMatches(
            frame,
            pattern: #"(?i)^\s*FRAME[ \t]+([0-9]+)\s*$"#
        )

        let raw: [String: Any] = [
            "engine": "Vision.framework VNRecognizeTextRequest",
            "frame": regionRecord(frame, region: frameRegion),
            "minimum_accepted_confidence": minimumConfidence,
            "normalization": "trim surrounding whitespace only; no character substitution or digit guessing",
            "timestamp": regionRecord(timestamp, region: timestampRegion),
        ]
        guard timestampValues.count == 1 else {
            return OCRResult(
                metadata: nil,
                failureReason: "timestamp parse produced \(timestampValues.count) distinct exact matches",
                rawRecord: raw
            )
        }
        guard frameValues.count == 1 else {
            return OCRResult(
                metadata: nil,
                failureReason: "FRAME parse produced \(frameValues.count) distinct exact matches",
                rawRecord: raw
            )
        }
        guard let sourceTimestamp = Double(timestampValues[0]), sourceTimestamp.isFinite else {
            return OCRResult(
                metadata: nil,
                failureReason: "timestamp exact match is not a finite number",
                rawRecord: raw
            )
        }
        return OCRResult(
            metadata: OCRParsedMetadata(
                sourceTimestampServerMS: sourceTimestamp,
                frameID: frameValues[0]
            ),
            failureReason: nil,
            rawRecord: raw
        )
    }

    static func crop(_ image: CGImage, region: CGRect) throws -> CGImage {
        let width = CGFloat(image.width)
        let height = CGFloat(image.height)
        let pixelRect = CGRect(
            x: region.minX * width,
            y: (1 - region.maxY) * height,
            width: region.width * width,
            height: region.height * height
        ).integral.intersection(CGRect(x: 0, y: 0, width: width, height: height))
        guard let result = image.cropping(to: pixelRect) else {
            throw NSError(
                domain: "capture-producer.ocr",
                code: 1,
                userInfo: [NSLocalizedDescriptionKey: "could not crop OCR region \(region)"]
            )
        }
        return result
    }

    static func syntheticImage(timestamp: String, frameID: String) throws -> CGImage {
        let width = 1600
        let height = 760
        guard let context = CGContext(
            data: nil,
            width: width,
            height: height,
            bitsPerComponent: 8,
            bytesPerRow: width * 4,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
        ) else {
            throw NSError(domain: "capture-producer.synthetic", code: 1)
        }
        context.setFillColor(CGColor(red: 17 / 255, green: 17 / 255, blue: 17 / 255, alpha: 1))
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        draw(timestamp, size: 105, bold: true, at: CGPoint(x: 35, y: 760 - 155), in: context)
        draw("FRAME \(frameID)", size: 52, bold: false, at: CGPoint(x: 35, y: 760 - 255), in: context)
        guard let image = context.makeImage() else {
            throw NSError(domain: "capture-producer.synthetic", code: 2)
        }
        return image
    }

    static func writePNG(_ image: CGImage, to url: URL) throws {
        guard let destination = CGImageDestinationCreateWithURL(
            url as CFURL,
            UTType.png.identifier as CFString,
            1,
            nil
        ) else {
            throw NSError(domain: "capture-producer.png", code: 1)
        }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination) else {
            throw NSError(domain: "capture-producer.png", code: 2)
        }
    }

    static func loadImage(_ url: URL) throws -> CGImage {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
            throw NSError(
                domain: "capture-producer.png",
                code: 3,
                userInfo: [NSLocalizedDescriptionKey: "could not read image: \(url.path)"]
            )
        }
        return image
    }

    private struct Candidate {
        let text: String
        let confidence: Float
        let boundingBox: CGRect
    }

    private static func observations(in image: CGImage, region: CGRect) throws -> [Candidate] {
        let request = VNRecognizeTextRequest()
        request.recognitionLevel = .accurate
        request.recognitionLanguages = ["en-US"]
        request.usesLanguageCorrection = false
        request.regionOfInterest = region
        let handler = VNImageRequestHandler(cgImage: image, orientation: .up)
        try handler.perform([request])
        return (request.results ?? []).flatMap { observation in
            observation.topCandidates(3).map { candidate in
                Candidate(
                    text: candidate.string,
                    confidence: candidate.confidence,
                    boundingBox: observation.boundingBox
                )
            }
        }
    }

    private static func uniqueMatches(_ candidates: [Candidate], pattern: String) -> [String] {
        guard let expression = try? NSRegularExpression(pattern: pattern) else { return [] }
        var matches = Set<String>()
        for candidate in candidates where candidate.confidence >= minimumConfidence {
            let range = NSRange(candidate.text.startIndex..., in: candidate.text)
            guard let match = expression.firstMatch(in: candidate.text, range: range),
                  match.numberOfRanges == 2,
                  let valueRange = Range(match.range(at: 1), in: candidate.text) else {
                continue
            }
            matches.insert(String(candidate.text[valueRange]))
        }
        return matches.sorted()
    }

    private static func regionRecord(_ candidates: [Candidate], region: CGRect) -> [String: Any] {
        [
            "candidates": candidates.map { candidate in
                [
                    "bounding_box": rectRecord(candidate.boundingBox),
                    "confidence": candidate.confidence,
                    "text": candidate.text,
                ] as [String: Any]
            },
            "recognized_text": candidates.map(\.text),
            "region_of_interest": rectRecord(region),
        ]
    }

    private static func rectRecord(_ rect: CGRect) -> [String: CGFloat] {
        ["height": rect.height, "width": rect.width, "x": rect.origin.x, "y": rect.origin.y]
    }

    private static func draw(
        _ text: String,
        size: CGFloat,
        bold: Bool,
        at point: CGPoint,
        in context: CGContext
    ) {
        let fontName = bold ? "Menlo-Bold" : "Menlo"
        let font = CTFontCreateWithName(fontName as CFString, size, nil)
        let attributes: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): font,
            NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor.white,
        ]
        let line = CTLineCreateWithAttributedString(
            NSAttributedString(string: text, attributes: attributes)
        )
        context.textPosition = point
        CTLineDraw(line, context)
    }
}
