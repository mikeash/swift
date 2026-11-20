// RUN: %target-run-simple-swift(-target %target-swift-5.9-abi-triple)
// REQUIRES: executable_test

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// Test the depth-limited conformance check and type lookup entry points on
// types nested deeply enough to run out of depth through each kind of
// recursion they draw on.

import StdlibUnittest

@_silgen_name("swift_conformsToProtocolWithExecutionContextAndMaxDepth")
func swift_conformsToProtocolWithExecutionContextAndMaxDepth(
  _ type: UnsafeRawPointer,
  _ protocolDescriptor: UnsafeRawPointer,
  _ context: UnsafeMutableRawPointer?,
  _ remainingDepth: UnsafeMutablePointer<Int>?
) -> UnsafeRawPointer?

@_silgen_name("swift_getTypeByMangledNameInContextInMetadataStateAndMaxDepth")
func swift_getTypeByMangledNameInContextInMetadataStateAndMaxDepth(
  _ metadataState: UInt,
  _ name: UnsafePointer<CChar>,
  _ length: UInt,
  _ context: UnsafeRawPointer?,
  _ args: UnsafePointer<UnsafeRawPointer?>?,
  _ remainingDepth: UnsafeMutablePointer<Int>?
) -> UnsafeRawPointer?

protocol P {}
struct Leaf: P {}
struct OtherLeaf: P {}
struct NotP {}

struct Nested<T> {}
extension Nested: P where T: P {}

struct Box<T: ~Copyable>: ~Copyable {}
extension Box: Copyable where T: Copyable {}

struct Wrapper<T> {}
extension Wrapper: P {}

struct Constrained<T: P> {}
struct Holder<T> {}

func nested(_ t: Any.Type, _ n: Int) -> Any.Type {
  func wrap<T>(_: T.Type) -> Any.Type { Nested<T>.self }
  return n <= 0 ? t : nested(_openExistential(t, do: wrap), n - 1)
}

func boxed(_ t: Any.Type, _ n: Int) -> Any.Type {
  func wrap<T>(_: T.Type) -> Any.Type { Box<T>.self }
  return n <= 0 ? t : boxed(_openExistential(t, do: wrap), n - 1)
}

func wrapper(_ t: Any.Type) -> Any.Type {
  func wrap<T>(_: T.Type) -> Any.Type { Wrapper<T>.self }
  return _openExistential(t, do: wrap)
}

func metadata(_ type: Any.Type) -> UnsafeRawPointer {
  unsafeBitCast(type, to: UnsafeRawPointer.self)
}

// The protocol descriptor follows the kind, flags, and protocol count in the
// existential metadata of `any P`.
let pDescriptor = metadata((any P).self)
  .load(fromByteOffset: MemoryLayout<UInt>.size + 8, as: UnsafeRawPointer.self)

func conforms(_ type: Any.Type,
              depth: UnsafeMutablePointer<Int>?) -> Bool {
  swift_conformsToProtocolWithExecutionContextAndMaxDepth(
    metadata(type), pDescriptor, nil, depth) != nil
}

func lookUp(_ name: String, context: UnsafeRawPointer? = nil,
            args: [UnsafeRawPointer?] = [], depth: inout Int)
    -> UnsafeRawPointer? {
  name.withCString { cName in
    args.withUnsafeBufferPointer { args in
      swift_getTypeByMangledNameInContextInMetadataStateAndMaxDepth(
        0, cName, UInt(name.utf8.count), context, args.baseAddress, &depth)
    }
  }
}

let nesting = 60
let shallow = 5
let ample = 1000

let suite = TestSuite("MaxRecursionDepth")

suite.test("conditional conformance") {
  let type = nested(Leaf.self, nesting)

  var depth = shallow
  expectFalse(conforms(type, depth: &depth))
  expectEqual(0, depth)

  depth = ample
  expectTrue(conforms(type, depth: &depth))
  expectEqual(ample, depth)

  depth = shallow
  expectTrue(conforms(type, depth: &depth))
  expectEqual(shallow, depth)
}

suite.test("conditional conformance, unlimited") {
  expectTrue(conforms(nested(Leaf.self, nesting + 1), depth: nil))
}

suite.test("negative is not a depth failure") {
  var depth = ample
  expectFalse(conforms(nested(NotP.self, nesting), depth: &depth))
  expectEqual(ample, depth)
}

suite.test("conditional Copyable") {
  // Checking conditional Copyable takes time exponential in the nesting.
  let type = wrapper(boxed(Leaf.self, 12))

  var depth = shallow
  expectFalse(conforms(type, depth: &depth))
  expectEqual(0, depth)

  depth = ample
  expectTrue(conforms(type, depth: &depth))
  expectEqual(ample, depth)
}

suite.test("type lookup by name") {
  let type = nested(Leaf.self, nesting)
  let name = _mangledTypeName(type)!

  var depth = shallow
  expectNil(lookUp(name, depth: &depth))
  expectEqual(0, depth)

  depth = ample
  expectEqual(metadata(type), lookUp(name, depth: &depth))
  expectEqual(ample, depth)
}

suite.test("type lookup through wrapper nodes") {
  let name = "Si" + String(repeating: "lu", count: nesting)

  var depth = shallow
  expectEqual(metadata(Int.self), lookUp(name, depth: &depth))
  expectEqual(shallow, depth)
}

// Reading the type descriptor out of the metadata needs a ptrauth strip on
// arm64e.
#if !_ptrauth(_arm64e)
suite.test("generic requirements of a looked-up type") {
  // Look up `Constrained<T>` in the context of `Holder<T>`, with T bound to
  // the deep type, so the lookup's own decoding stays shallow and its depth
  // goes to checking `T: P`.
  let holderDescriptor = metadata(Holder<Int>.self)
    .load(fromByteOffset: MemoryLayout<UInt>.size, as: UnsafeRawPointer.self)
  let arg = metadata(nested(OtherLeaf.self, nesting))
  let name = "4main11ConstrainedVyxG"

  var depth = shallow + 5
  expectNil(lookUp(name, context: holderDescriptor, args: [arg],
                   depth: &depth))
  expectEqual(0, depth)

  depth = ample
  expectNotNil(lookUp(name, context: holderDescriptor, args: [arg],
                      depth: &depth))
  expectEqual(ample, depth)
}
#endif

runAllTests()
