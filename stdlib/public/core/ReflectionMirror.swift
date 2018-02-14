
@_silgen_name("swift_reflectionMirror_normalizedType")
private func _getNormalizedType<T>(_: T) -> Any.Type

@_silgen_name("swift_reflectionMirror_count")
private func _getChildCount<T>(_: T, type: Any.Type) -> Int

private typealias NameFreeFunc = @convention(c) (UnsafePointer<CChar>?) -> Void

@_silgen_name("swift_reflectionMirror_subscript")
private func _getChild<T>(
  of: T,
  type: Any.Type,
  index: Int,
  outName: UnsafeMutablePointer<UnsafePointer<CChar>?>,
  outFreeFunc: UnsafeMutablePointer<NameFreeFunc?>
) -> Any

// Returns 'c' (class), 'e' (enum), 's' (struct), 't' (tuple), or '\0' (none)
@_silgen_name("swift_reflectionMirror_displayStyle")
private func _getDisplayStyle<T>(_: T) -> CChar

private func getChild<T>(of value: T, type: Any.Type, index: Int) -> (label: String?, value: Any) {
  var nameC: UnsafePointer<CChar>? = nil
  var freeFunc: NameFreeFunc? = nil
  
  let value = _getChild(of: value, type: type, index: index, outName: &nameC, outFreeFunc: &freeFunc)
  
  let name = nameC.flatMap({ String(validatingUTF8: $0) })
  freeFunc?(nameC)
  return (name, value)
}

#if _runtime(_ObjC)
@_silgen_name("swift_reflectionMirror_quickLookObject")
private func _getQuickLookObject<T>(_: T) -> AnyObject?

@_inlineable // FIXME(sil-serialize-all)
@_versioned // FIXME(sil-serialize-all)
@_silgen_name("_swift_stdlib_NSObject_isKindOfClass")
internal func _isImpl(_ object: AnyObject, kindOf: AnyObject) -> Bool

@_inlineable // FIXME(sil-serialize-all)
@_versioned // FIXME(sil-serialize-all)
internal func _is(_ object: AnyObject, kindOf `class`: String) -> Bool {
  return _isImpl(object, kindOf: `class` as AnyObject)
}

@_inlineable // FIXME(sil-serialize-all)
@_versioned // FIXME(sil-serialize-all)
internal func _getClassPlaygroundQuickLook(
  _ object: AnyObject
) -> PlaygroundQuickLook? {
  if _is(object, kindOf: "NSNumber") {
    let number: _NSNumber = unsafeBitCast(object, to: _NSNumber.self)
    switch UInt8(number.objCType[0]) {
    case UInt8(ascii: "d"):
      return .double(number.doubleValue)
    case UInt8(ascii: "f"):
      return .float(number.floatValue)
    case UInt8(ascii: "Q"):
      return .uInt(number.unsignedLongLongValue)
    default:
      return .int(number.longLongValue)
    }
  } else if _is(object, kindOf: "NSAttributedString") {
    return .attributedString(object)
  } else if _is(object, kindOf: "NSImage") ||
            _is(object, kindOf: "UIImage") ||
            _is(object, kindOf: "NSImageView") ||
            _is(object, kindOf: "UIImageView") ||
            _is(object, kindOf: "CIImage") ||
            _is(object, kindOf: "NSBitmapImageRep") {
    return .image(object)
  } else if _is(object, kindOf: "NSColor") ||
            _is(object, kindOf: "UIColor") {
    return .color(object)
  } else if _is(object, kindOf: "NSBezierPath") ||
            _is(object, kindOf: "UIBezierPath") {
    return .bezierPath(object)
  } else if _is(object, kindOf: "NSString") {
    return .text(_forceBridgeFromObjectiveC(object, String.self))
  }

  return .none
}
#endif

extension Mirror {
  public init(internalReflecting subject: Any, subjectType: Any.Type? = nil) {
		let subjectType = subjectType ?? _getNormalizedType(subject)
    
    let childCount = _getChildCount(subject, type: subjectType)
    let children = (0 ..< childCount).lazy.map({ getChild(of: subject, type: subjectType, index: $0) })
    self.children = Children(children)
    
    self._makeSuperclassMirror = {
      if let subjectClass = subjectType as? AnyClass,
         let superclass = _getSuperclass(subjectClass) {
        return Mirror(internalReflecting: subject, subjectType: superclass)
      } else {
        return nil
      }
    }
    
    let rawDisplayStyle = _getDisplayStyle(subject)
    switch UnicodeScalar(Int(rawDisplayStyle)) {
    case "c": self.displayStyle = .class
    case "e": self.displayStyle = .enum
    case "s": self.displayStyle = .struct
    case "t": self.displayStyle = .tuple
    case "\0": self.displayStyle = nil
    default: preconditionFailure("Unknown raw display style '\(rawDisplayStyle)'")
    }
    
    self.subjectType = subjectType
    self._defaultDescendantRepresentation = .generated
  }
  
  public static func quickLookObject(_ subject: Any) -> PlaygroundQuickLook? {
#if _runtime(_ObjC)
    let object = _getQuickLookObject(subject)
    return object.flatMap(_getClassPlaygroundQuickLook)
#else
    return nil
#endif
  }
}
