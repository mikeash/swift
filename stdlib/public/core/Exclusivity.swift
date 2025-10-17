fileprivate let ValueBufferSize = 3 * MemoryLayout<UnsafeRawPointer>.stride

fileprivate struct Access: ~Copyable {
  enum Action {
    case read
    case modify
  }
  struct NextAndAction {
    private static let actionMask: UInt = 0x1
    private var rawValue: UnsafeRawPointer?


  }

  var pointer: UnsafeRawPointer?
  var pc: UnsafeRawPointer?
  var nextAndAction: NextAndAction

  static func from(rawPointer: UnsafeMutableRawPointer) -> UnsafeMutablePointer<Access> {
    return rawPointer.assumingMemoryBound(to: Access.self)
  }
}

@_extern(c, "_swift_getExclusivityTLS")
fileprivate func _swift_getExclusivityTLS() -> UnsafeMutableRawPointer?

@_extern(c, "_swift_setExclusivityTLS")
fileprivate func _swift_setExclusivityTLS(_:UnsafeMutableRawPointer?)

@_cdecl("swift_beginAccess")
@usableFromInline
internal func swift_beginAccess(
  pointer: UnsafeRawPointer,
  buffer: UnsafeMutableRawPointer,
  flags: UInt,
  pc: UnsafeRawPointer) {
  precondition(MemoryLayout<Access>.size <= ValueBufferSize)

  var access = Access.from(rawPointer: buffer);
  access.pointee.pointer = pointer
  access.pointee.pc = UnsafeRawPointer(_swift_getExclusivityTLS())
  _swift_setExclusivityTLS(buffer)
}

@_cdecl("swift_endAccess")
@usableFromInline
internal func swift_endAccess(buffer: UnsafeMutableRawPointer) {
  _swift_setExclusivityTLS(nil)
}
