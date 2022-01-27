import SwiftRemoteMirror

func dumpConcurrency(
  context: SwiftReflectionContextRef,
  inspector: Inspector
) throws {
  let dumper = ConcurrencyDumper(context: context, inspector: inspector)
  dumper.dumpTasks()
  dumper.dumpActors()
  dumper.dumpThreads()
}

fileprivate class ConcurrencyDumper {
  let context: SwiftReflectionContextRef
  let inspector: Inspector
  let jobMetadata: swift_reflection_ptr_t?
  let taskMetadata: swift_reflection_ptr_t?

  struct TaskInfo {
    var address: swift_reflection_ptr_t
    var flags: UInt32
    var id: UInt64
    var runJob: swift_reflection_ptr_t
    var allocatorSlabPtr: swift_reflection_ptr_t
    var allocatorTotalSize: Int
    var allocatorTotalChunks: Int
    var childTasks: [swift_reflection_ptr_t]
    var parent: swift_reflection_ptr_t?
  }

  struct HeapInfo {
    var tasks: [swift_reflection_ptr_t] = []
    var jobs: [swift_reflection_ptr_t] = []
    var actors: [swift_reflection_ptr_t] = []
  }

  lazy var heapInfo: HeapInfo = gatherHeapInfo()

  lazy var threadCurrentTasks = inspector.threadCurrentTasks().filter{ $0.currentTask != 0 }

  lazy var tasks: [swift_reflection_ptr_t: TaskInfo] = gatherTasks()

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

  func gatherTasks() -> [swift_reflection_ptr_t: TaskInfo] {
    var map: [swift_reflection_ptr_t: TaskInfo] = [:]
    var tasksToScan: Set<swift_reflection_ptr_t> = []
    tasksToScan.formUnion(heapInfo.tasks)
    tasksToScan.formUnion(threadCurrentTasks.map{ $0.currentTask }.filter{ $0 != 0 })

    while !tasksToScan.isEmpty {
      let taskToScan = tasksToScan.removeFirst()
      if let info = info(forTask: taskToScan) {
        map[taskToScan] = info
        for child in info.childTasks {
          let childMetadata = swift_reflection_metadataForObject(context, UInt(child))
          if let taskMetadata = taskMetadata, childMetadata != taskMetadata {
            print("Inconsistent data datected!! Child task \(hex: child) has unknown metadata \(hex: taskMetadata)")
          }
          if map[child] == nil {
            tasksToScan.insert(child)
          }
        }
      }
    }

    for (task, info) in map {
      for child in info.childTasks {
        map[child]!.parent = task
      }
    }

    return map
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

  func info(forTask task: swift_reflection_ptr_t) -> TaskInfo? {
    let reflectionInfo = swift_reflection_asyncTaskInfo(context, task)
    if let error = reflectionInfo.Error {
      print("Error getting info for async task \(hex: task): \(String(cString: error))")
      return nil
    }

    // ChildTasks is a temporary pointer which we must copy out before we call
    // into Remote Mirror again.
    let children = Array(UnsafeBufferPointer(
        start: reflectionInfo.ChildTasks,
        count: Int(reflectionInfo.ChildTaskCount)))

    var allocatorSlab = reflectionInfo.AllocatorSlabPtr
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

    return TaskInfo(
      address: task,
      flags: reflectionInfo.Flags,
      id: reflectionInfo.Id,
      runJob: reflectionInfo.RunJob,
      allocatorSlabPtr: reflectionInfo.AllocatorSlabPtr,
      allocatorTotalSize: allocatorTotalSize,
      allocatorTotalChunks: allocatorTotalChunks,
      childTasks: children
    )
  }

  func dumpTasks() {
    print("TASKS")

    for task in tasks.keys.sorted() {
      let info = tasks[task]!
      let runJobSymbol = inspector.getSymbol(address: info.runJob)
      let runJobName = runJobSymbol.name ?? "<\(hex: info.runJob)>"
      let runJobLibrary = runJobSymbol.library ?? "<unknown>"

      let childrenHex = info.childTasks.map{ "\(hex: $0)" }

      print("  \(hex: task) - flags=\(hex: info.flags) id=\(info.id)")
      if let parent = info.parent {
        print("    parent: \(hex: parent)")
      }
      print("    resume function: \(runJobName) in \(runJobLibrary)")
      print("    task allocator: \(info.allocatorTotalSize) bytes in \(info.allocatorTotalChunks) chunks")
      if !childrenHex.isEmpty {
        print("    children:", childrenHex.joined(separator: ", "))
      }
    }

    print("")
  }

  func dumpActors() {
    print("ACTORS")

    for actor in actors {
      let metadata = swift_reflection_metadataForObject(context, UInt(actor))
      let metadataName = name(metadata: swift_reflection_ptr_t(metadata)) ?? "<unknown class name>"
      let info = swift_reflection_actorInfo(context, actor);
      print("  \(hex: actor) \(metadataName) flags=\(hex: info.Flags)")

      var job = info.FirstJob
      if job == 0 {
        print("    empty job queue")
      } else {
        print("    job queue: \(hex: job)", terminator: "")
        while job != 0 {
          job = swift_reflection_nextJob(context, job);
          if job != 0 {
            print(" -> \(hex: job)", terminator: "")
          }
        }
        print("")
      }
    }

    print("")
  }

  func dumpThreads() {
    print("THREADS")
    if threadCurrentTasks.isEmpty {
      print("  no threads with active tasks")
      return
    }

    for (thread, task) in threadCurrentTasks {
      print("  Thread \(hex: thread) - current task: \(task)")
    }
  }
}
