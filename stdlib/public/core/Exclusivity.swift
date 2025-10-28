fileprivate let ValueBufferSize = unsafe 3 * MemoryLayout<UnsafeRawPointer>.stride

fileprivate let TrackingFlag: UInt = 0x20
fileprivate let ActionMask: UInt = 0x1

fileprivate typealias AccessPointer = UnsafeMutablePointer<Access>
@unsafe fileprivate struct Access: ~Copyable {

  enum Action: UInt {
    case read
    case modify
  }

  struct NextAndAction {
    private var rawValue: UInt

    var action: Action {
      get {
        Action(rawValue: rawValue & ActionMask)!
      }
      set {
        rawValue = (rawValue & ~ActionMask) | newValue.rawValue
      }
    }

    var next: AccessPointer? {
      get { unsafe UnsafeMutablePointer(bitPattern: rawValue & ~ActionMask) }
      set { rawValue = UInt(bitPattern: newValue) | (rawValue & ActionMask) }
    }
  }

  var pointer: UnsafeRawPointer?
  var pc: UnsafeRawPointer?
  var nextAndAction: NextAndAction

  var action: Action {
    get { unsafe nextAndAction.action }
    set { unsafe nextAndAction.action = newValue }
  }

  var next: AccessPointer? {
    get { unsafe nextAndAction.next }
    set { unsafe nextAndAction.next = newValue }
  }

  static func from(rawPointer: UnsafeMutableRawPointer?) -> AccessPointer? {
    guard let rawPointer = unsafe rawPointer else { return nil }
    return unsafe rawPointer.assumingMemoryBound(to: Access.self)
  }

  static func search(access: AccessPointer, inserting: Bool, head: inout AccessPointer?) {
    if let head = unsafe head {
      if unsafe head.pointee.pointer == access.pointee.pointer {
        if unsafe head.pointee.action == Action.modify || access.pointee.action == Action.modify {
          fatalError("exclusive access collision eep!")
        }
      }
      unsafe search(access: access, inserting: inserting, head: &head.pointee.next)
    } else if inserting {
      unsafe access.pointee.next = nil
      unsafe head = access
    }
  }

  static func remove(access: AccessPointer, head: inout AccessPointer?) {
    if unsafe head == nil {
      unsafe fatalError("Didn't find exclusive access buffer \(access)")
    } else if unsafe head == access {
      unsafe head = access.pointee.next
    } else {
      unsafe remove(access: access, head: &head!.pointee.next)
    }
  }
}

@_extern(c, "_swift_getExclusivityTLS")
fileprivate func _swift_getExclusivityTLS() -> UnsafeMutableRawPointer?

@_extern(c, "_swift_setExclusivityTLS")
fileprivate func _swift_setExclusivityTLS(_:UnsafeMutableRawPointer?)

fileprivate var accessHead: AccessPointer? {
  get { unsafe Access.from(rawPointer: _swift_getExclusivityTLS()) }
  set { unsafe _swift_setExclusivityTLS(newValue) }
}

@_cdecl("swift_beginAccess")
@usableFromInline
@unsafe
internal func swift_beginAccess(
  pointer: UnsafeRawPointer,
  buffer: UnsafeMutableRawPointer,
  flags: UInt,
  pc: UnsafeRawPointer) {
  precondition(unsafe MemoryLayout<Access>.size <= ValueBufferSize)

  guard let access = unsafe Access.from(rawPointer: buffer) else {
    fatalError("NULL access buffer")
  }

  guard let action = Access.Action(rawValue: flags & ActionMask) else {
    fatalError("Unable to construct action from flags \(flags)")
  }

  unsafe access.pointee.pointer = pointer
  unsafe access.pointee.pc = nil
  unsafe access.pointee.action = action

  let isTracking = (flags & TrackingFlag) != 0

  unsafe Access.search(access: access, inserting: isTracking, head: &accessHead)
}

@_cdecl("swift_endAccess")
@usableFromInline
@unsafe
internal func swift_endAccess(buffer: UnsafeMutableRawPointer) {
  guard let access = unsafe Access.from(rawPointer: buffer) else {
    fatalError("NULL access buffer")
  }
  unsafe Access.remove(access: access, head: &accessHead)
}
