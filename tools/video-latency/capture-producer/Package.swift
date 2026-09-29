// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "capture-producer",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "capture-producer", targets: ["CaptureProducer"]),
    ],
    targets: [
        .executableTarget(name: "CaptureProducer"),
    ]
)
