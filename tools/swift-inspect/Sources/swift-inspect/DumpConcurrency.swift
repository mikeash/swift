import SwiftRemoteMirror

func dumpConcurrency(
  context: SwiftReflectionContextRef,
  inspector: Inspector
) throws {
  let dumper = ConcurrencyDumper(context: context, inspector: inspector)
  dumper.dumpTasks()
  dumper.dumpActors()
}

fileprivate class ConcurrencyDumper {
  let context: SwiftReflectionContextRef
  let inspector: Inspector
  let jobMetadata: swift_reflection_ptr_t?
  let taskMetadata: swift_reflection_ptr_t?

  struct HeapInfo {
    var tasks: [swift_reflection_ptr_t] = []
    var jobs: [swift_reflection_ptr_t] = []
    var actors: [swift_reflection_ptr_t] = []
  }

  lazy var heapInfo: HeapInfo = gatherHeapInfo()

  var tasks: [swift_reflection_ptr_t] {
    heapInfo.tasks
  }

  var actors: [swift_reflection_ptr_t] {
    heapInfo.actors
  }

  var metadataIsActorCache: [swift_reflection_ptr_t: Bool] = [:]
  var metadataNameCache: [swift_reflection_ptr_t: String?] = [:]

  init(context: SwiftReflectionContextRef, inspector: Inspector) {
    self.context = context
    self.inspector = inspector

    func getMetadata(symbolName: String) -> swift_reflection_ptr_t? {
      let addr = inspector.getAddr(symbolName: symbolName)
      if let ptr = inspector.read(address: addr, size: MemoryLayout<UInt>.size) {
        return swift_reflection_ptr_t(ptr.load(as: UInt.self))
      }
      return nil
    }
    jobMetadata = getMetadata(symbolName: "_swift_concurrency_debug_jobMetadata")
    taskMetadata = getMetadata(symbolName: "_swift_concurrency_debug_asyncTaskMetadata")
  }

  func gatherHeapInfo() -> HeapInfo {
    var result = HeapInfo()
    
    inspector.enumerateMallocs { (pointer, size) in
      let metadata = swift_reflection_ptr_t(swift_reflection_metadataForObject(context, UInt(pointer)))
      if metadata == jobMetadata {
        result.jobs.append(pointer)
      } else if metadata == taskMetadata {
        result.tasks.append(pointer)
      } else if isActorMetadata(metadata) {
        result.actors.append(pointer)
      }
    }

    return result
  }

  func isActorMetadata(_ metadata: swift_reflection_ptr_t) -> Bool {
    if let cached = metadataIsActorCache[metadata] {
      return cached
    }
    let result = swift_reflection_metadataIsActor(context, metadata) != 0
    metadataIsActorCache[metadata] = result
    return result
  }

  func name(metadata: swift_reflection_ptr_t) -> String? {
    if let cached = metadataNameCache[metadata] {
      return cached
    }

    let name = context.name(metadata: metadata)
    metadataNameCache[metadata] = name
    return name
  }

  func dumpTasks() {
    print("TASKS")

    for task in tasks {
      let info = swift_reflection_asyncTaskInfo(context, task)
      guard info.Error == nil else { continue }

      let runJobName = inspector.getSymbol(address: info.RunJob).name
        ?? "<\(hex: info.RunJob)>"

      var allocatorSlab = info.AllocatorSlabPtr
      var allocatorTotalSize = 0
      var allocatorTotalChunks = 0
      while allocatorSlab != 0 {
        let allocations = swift_reflection_asyncTaskSlabAllocations(context,
                                                                    allocatorSlab)
        guard allocations.Error == nil else { break }
        allocatorTotalSize += Int(allocations.SlabSize)
        allocatorTotalChunks += Int(allocations.ChunkCount)

        allocatorSlab = allocations.NextSlab
      }

      print("  \(hex: task) - flags=\(hex: info.Flags) id=\(info.Id) runjob=\(runJobName)")
      print("    task allocator: \(allocatorTotalSize) bytes in \(allocatorTotalChunks) chunks")
    }

    print("")
  }

  func dumpActors() {
    print("ACTORS")

    for actor in actors {
      let metadata = swift_reflection_metadataForObject(context, UInt(actor))
      let metadataName = name(metadata: swift_reflection_ptr_t(metadata)) ?? "<unknown class name>"
      print("  \(hex: actor) \(metadataName)")
    }

    print("")
  }
}
