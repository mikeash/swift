// RUN: %target-run-simple-swift(-import-objc-header %S/Inputs/custom-rr-abi-utilities.h)

// REQUIRES: CPU=arm64 || CPU=arm64e

// REQUIRES: executable_test
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import StdlibUnittest

class RetainReleaseChecker {
  var pointerValue: UnsafeMutableRawPointer

  private class Helper {}

  private weak var weakRef: Helper?

  init() {
    let helper = Helper()
    pointerValue = Unmanaged.passRetained(helper).toOpaque
    weakRef = helper
  }
}

var CustomRRABITestSuite("CustomRRABI")

CustomRRABITestSuite.test("retain") {

}

runAllTests()
