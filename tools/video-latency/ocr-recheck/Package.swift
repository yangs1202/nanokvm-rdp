// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "ocr-recheck",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "ocr-recheck", targets: ["OCRRecheck"]),
    ],
    targets: [
        .executableTarget(name: "OCRRecheck"),
    ]
)
