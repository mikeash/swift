import SwiftShims

fileprivate let ValueBufferSize = unsafe 3 * MemoryLayout<UnsafeRawPointer>.stride

fileprivate let TrackingFlag: UInt = 0x20
fileprivate let ActionMask: UInt = 0x1

fileprivate typealias AccessPointer = UnsafeMutablePointer<Access>
@unsafe fileprivate struct Access {

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

  var location: UnsafeRawPointer?
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

  @inline(__always)
  static func search(access: AccessPointer, inserting: Bool, head: inout AccessPointer?) {
    var cursor = unsafe head
    while let nextPtr = unsafe cursor {
      if unsafe nextPtr.pointee.location == access.pointee.location {
        if unsafe nextPtr.pointee.action == Action.modify || access.pointee.action == Action.modify {
          simultaneousAccess()
        }
      }
      unsafe cursor = nextPtr.pointee.next
    }

    if inserting {
      unsafe access.pointee.next = head
      unsafe head = access
    }
  }

  @inline(__always)
  static func remove(access: AccessPointer, head: inout AccessPointer?) {
    var cursor = unsafe head
    var previous: AccessPointer? = nil
    while let nextPtr = unsafe cursor {
      if unsafe nextPtr == access {
        if let previous = unsafe previous {
          unsafe previous.pointee.next = access.pointee.next
        } else {
          unsafe head = access.pointee.next
        }
        return
      }
      unsafe previous = nextPtr
      unsafe cursor = nextPtr.pointee.next
    }

    unsafe accessNotFound(access)
  }
}

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
    nullAccessBuffer()
  }

  guard let action = Access.Action(rawValue: flags & ActionMask) else {
    invalidFlags(flags)
  }

  unsafe access.pointee.location = pointer
  unsafe access.pointee.pc = pc
  unsafe access.pointee.action = action

  let isTracking = (flags & TrackingFlag) != 0

  unsafe Access.search(access: access, inserting: isTracking, head: &accessHead)
}

@_cdecl("swift_endAccess")
@usableFromInline
@unsafe
internal func swift_endAccess(buffer: UnsafeMutableRawPointer) {
  guard let access = unsafe Access.from(rawPointer: buffer) else {
    nullAccessBuffer()
  }
  unsafe Access.remove(access: access, head: &accessHead)
}

@inline(never)
fileprivate func simultaneousAccess() -> Never {
  fatalError("exclusive access collision eep!")
}

@inline(never)
fileprivate func invalidFlags(_ flags: UInt) -> Never {
  fatalError("Unable to construct action from flags \(flags)")
}

@inline(never)
fileprivate func accessNotFound(_ access: AccessPointer) -> Never {
  unsafe fatalError("Didn't find exclusive access buffer \(access)")
}

@inline(never)
fileprivate func nullAccessBuffer() -> Never {
  fatalError("NULL access buffer")
}
