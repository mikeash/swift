// swift-tools-version:5.2
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription
import class Foundation.ProcessInfo

let package = Package(
    name: "swift-inspect",
    targets: [
        // Targets are the basic building blocks of a package. A target can define a module or a test suite.
        // Targets can depend on other targets in this package, and on products in packages which this package depends on.
        .target(
            name: "swift-inspect",
            dependencies: [
                "SymbolicationShims",
                .product(name: "ArgumentParser", package: "swift-argument-parser"),
            ]),
        .testTarget(
            name: "swiftInspectTests",
            dependencies: ["swift-inspect"]),
        .systemLibrary(
            name: "SymbolicationShims")
    ]
)

if ProcessInfo.processInfo.environment["SWIFTCI_USE_LOCAL_DEPS"] == nil {
  package.dependencies += [
      .package(url: "https://github.com/apple/swift-argument-parser", from: "0.0.1"),
  ]
} else {
  package.dependencies += [
      .package(path: "../../../swift-argument-parser")
  ]
}
