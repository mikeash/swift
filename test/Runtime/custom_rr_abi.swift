// RUN: %target-run-simple-swift(-import-objc-header %S/Inputs/custom_rr_abi_utilities.h)

// REQUIRES: CPU=arm64 || CPU=arm64e

// REQUIRES: executable_test
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import StdlibUnittest

class RetainReleaseChecker {
  var pointerValue: UnsafeMutableRawPointer

  private class Helper {}

  private weak var weakRef: Helper?

  private let originalRetainCount: UInt

  init() {
    do {
      let helper = Helper()
      pointerValue = Unmanaged.passRetained(helper).toOpaque()
      weakRef = helper
    }
    originalRetainCount = _getRetainCount(weakRef!)
  }

  var retained: Bool {
    weakRef != nil && _getRetainCount(weakRef!) > originalRetainCount
  }

  var released: Bool {
    weakRef == nil
  }
}

var CustomRRABITestSuite = TestSuite("CustomRRABI")

CustomRRABITestSuite.test("retain") {
  foreachRRFunction { function, cname, register, isRetain in
    let name = String(cString: cname!)
    let fullname = "\(name)_x\(register)"
    print("Testing \(name)_x\(register)")
    var checkers = (0..<NUM_REGS).map{ _ in RetainReleaseChecker() }
    var regs: [UnsafeMutableRawPointer?] = checkers.map{ $0.pointerValue }

    function!(&regs)

    for (i, checker) in checkers.enumerated() {
      if i == register {
        if isRetain != 0 {
          expectTrue(checker.retained, "\(fullname) must retain x\(i)")
        } else {
          expectTrue(checker.released, "\(fullname) must release x\(i)")
        }
      } else {
        expectFalse(checker.retained, "\(fullname) must not retain x\(i)")
        expectFalse(checker.released, "\(fullname) must not retain x\(i)")
      }
    }
  }
}

runAllTests()
