//===--- GenericMetadataBuilder.cpp - Code to build generic metadata. -----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Builder for generic metadata, in-process and out-of-process.
//
//===----------------------------------------------------------------------===//

#include "MetadataCache.h"
#include "Private.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/TargetLayout.h"
#include "swift/Runtime/Metadata.h"
#include "llvm/Support/Casting.h"
#include <dlfcn.h>
#include <inttypes.h>
#include <string>

using namespace swift;

#define LOG_ENABLED 1
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#define LOG(fmt, ...)                                                          \
  fprintf(stderr, "%s:%d:%s: " fmt "\n", __FILE_NAME__, __LINE__,              \
          __func__ __VA_OPT__(, ) __VA_ARGS__)

template <class T>
static constexpr T roundUpToAlignment(T offset, T alignment) {
  return (offset + alignment - 1) & ~(alignment - 1);
}

static size_t roundUpToAlignMask(size_t size, size_t alignMask) {
  return (size + alignMask) & ~alignMask;
}

static constexpr uint64_t sizeWithAlignmentMask(uint64_t size,
                                                uint64_t alignmentMask,
                                                uint64_t hasExtraInhabitants) {
  return (hasExtraInhabitants << 48) | (size << 16) | alignmentMask;
}

class InProcessReaderWriter {
public:
  using Runtime = InProcess;

  using Size = typename Runtime::StoredSize;
  using StoredPointer = typename Runtime::StoredPointer;
  using GenericArgument = void *;

  template <typename T>
  class Buffer;

  class ReadableData {
  public:
    void *ptr;
    size_t size;
  };

  template <typename T>
  class Buffer {
  public:
    Buffer(T *ptr) : ptr(ptr) {}

    Buffer(Buffer<const char> buffer)
        : ptr(reinterpret_cast<T *>(buffer.ptr)) {}

    T *ptr;

    T &operator[](size_t i) { return ptr[i]; }

    operator bool() const { return ptr; }

    Buffer<char> resolvePointer(uintptr_t *ptr) {
      return {reinterpret_cast<char *>(*ptr)};
    }

    template <typename U, bool Nullable>
    Buffer<U> resolvePointer(const RelativeDirectPointer<U, Nullable> *ptr) {
      return {ptr->get()};
    }

    template <typename U, bool Nullable>
    Buffer<U>
    resolvePointer(const RelativeIndirectablePointer<U, Nullable> *ptr) {
      return {ptr->get()};
    }

    template <typename U, bool Nullable>
    Buffer<const U>
    resolvePointer(const RelativeIndirectablePointer<const U, Nullable> *ptr) {
      return {ptr->get()};
    }

    template <typename U>
    Buffer<const U> resolvePointer(const U *const *ptr) {
      return {*ptr};
    }

    template <typename U>
    Buffer<const char> resolveFunctionPointer(const U *ptr) {
      return {reinterpret_cast<const char *>(*ptr)};
    }

    template <typename U, bool nullable>
    Buffer<const char> resolveFunctionPointer(
        TargetCompactFunctionPointer<Runtime, U, nullable> *ptr) {
      return {reinterpret_cast<const char *>(ptr->get())};
    }

    uint64_t getAddress() { return (uint64_t)ptr; }
  };

  template <typename T>
  class WritableData : public Buffer<T> {
    void checkPtr(void *toCheck) {
      assert((uintptr_t)toCheck - (uintptr_t)this->ptr < size);
    }

  public:
    WritableData(T *ptr, size_t size) : Buffer<T>(ptr), size(size) {}

    size_t size;
    size_t cursor;

    template <typename U>
    void writePointer(StoredPointer *to, Buffer<U> target) {
      checkPtr(to);
      *to = reinterpret_cast<StoredPointer>(target.ptr);
    }

    template <typename U>
    void writePointer(U **to, Buffer<U> &target) {
      checkPtr(to);
      *to = target.ptr;
    }

    template <typename U>
    void writePointer(const U **to, Buffer<U> &target) {
      checkPtr(to);
      *to = target.ptr;
    }

    void writePointer(GenericArgument *to, GenericArgument target) {
      checkPtr(to);
      *to = target;
    }

    void writeFunctionPointer(void *to, Buffer<const char> target) {
      checkPtr(to);
      *reinterpret_cast<const void **>(to) = target.ptr;
    }
  };

  struct SymbolInfo {
    std::string symbolName;
    std::string libraryName;
    uint64_t pointerOffset;
  };

  template <typename T>
  SymbolInfo getSymbolInfo(Buffer<T> buffer) {
    Dl_info info;
    int result = dladdr(buffer.ptr, &info);
    if (result == 0)
      return {"<unknown>", "<unknown>", 0};

    if (info.dli_fname == nullptr)
      info.dli_fname = "<unknown>";
    if (info.dli_sname == nullptr)
      info.dli_sname = "<unknown>";

    const char *libName = info.dli_fname;
    if (auto slash = strrchr(libName, '/'))
      libName = slash + 1;

    return {info.dli_sname, libName,
            buffer.getAddress() - (uintptr_t)info.dli_fbase};
  }

  Buffer<const char> getSymbolPointer(const char *name) {
    void *ptr = dlsym(RTLD_SELF, name);
    LOG("getSymbolPointer(\"%s\") -> %p", name, ptr);
    return {reinterpret_cast<const char *>(ptr)};
  }

  template <typename T>
  WritableData<T> allocate(size_t size);
  template <typename T>
  T *read(ReadableData &data, size_t offset);
  void write();
};

template <typename T>
InProcessReaderWriter::WritableData<T>
InProcessReaderWriter::allocate(size_t size) {
  auto bytes = reinterpret_cast<T *>(
      MetadataAllocator(MetadataAllocatorTags::GenericValueMetadataTag)
          .Allocate(size, alignof(void *)));

  return WritableData<T>{bytes, size};
}

template <typename ReaderWriter>
class GenericMetadataBuilder {
  using Runtime = typename ReaderWriter::Runtime;

  template <typename T>
  using Pointer = typename ReaderWriter::Runtime::template Pointer<T>;

  template <typename T>
  using Buffer = typename ReaderWriter::template Buffer<T>;

  using Size = typename ReaderWriter::Size;

  template <typename T>
  using WritableData = typename ReaderWriter::template WritableData<T>;

  using StoredPointer = typename ReaderWriter::Runtime::StoredPointer;

  using StoredSize = typename Runtime::StoredSize;

  using GenericArgument = typename ReaderWriter::GenericArgument;

  ReaderWriter readerWriter;

  Buffer<const char> pod_copy;
  Buffer<const char> pod_destroy;
  Buffer<const char> pod_direct_initializeBufferWithCopyOfBuffer;
  Buffer<const char> pod_indirect_initializeBufferWithCopyOfBuffer;

  Buffer<const TargetValueWitnessTable<Runtime>> VWT_Bi8_;
  Buffer<const TargetValueWitnessTable<Runtime>> VWT_Bi16_;
  Buffer<const TargetValueWitnessTable<Runtime>> VWT_Bi32_;
  Buffer<const TargetValueWitnessTable<Runtime>> VWT_Bi64_;
  Buffer<const TargetValueWitnessTable<Runtime>> VWT_Bi128_;
  Buffer<const TargetValueWitnessTable<Runtime>> VWT_Bi256_;
  Buffer<const TargetValueWitnessTable<Runtime>> VWT_Bi512_;

  template <typename DescriptorType>
  const char *getDescriptorName(Buffer<DescriptorType> descriptionBuffer) {
    auto name = descriptionBuffer.resolvePointer(&descriptionBuffer.ptr->Name);
    return name.ptr ? name.ptr : "<unknown>";
  }

  template <typename Word>
  static Word *wordsOffset(void *from, size_t offset) {
    auto asWords = reinterpret_cast<Word *>(from);
    return asWords + offset;
  }

  template <typename Word>
  static const Word *wordsOffset(const void *from, size_t offset) {
    auto asWords = reinterpret_cast<const Word *>(from);
    return asWords + offset;
  }

public:
  GenericMetadataBuilder(ReaderWriter readerWriter)
      : readerWriter(readerWriter),
        pod_copy(readerWriter.getSymbolPointer("_swift_pod_copy")),
        pod_destroy(readerWriter.getSymbolPointer("_swift_pod_destroy")),
        pod_direct_initializeBufferWithCopyOfBuffer(
            readerWriter.getSymbolPointer(
                "_swift_pod_direct_initializeBufferWithCopyOfBuffer")),
        pod_indirect_initializeBufferWithCopyOfBuffer(
            readerWriter.getSymbolPointer(
                "_swift_pod_indirect_initializeBufferWithCopyOfBuffer")),
        VWT_Bi8_(readerWriter.getSymbolPointer(
            MANGLE_AS_STRING(VALUE_WITNESS_SYM(Bi8_)))),
        VWT_Bi16_(readerWriter.getSymbolPointer(
            MANGLE_AS_STRING(VALUE_WITNESS_SYM(Bi16_)))),
        VWT_Bi32_(readerWriter.getSymbolPointer(
            MANGLE_AS_STRING(VALUE_WITNESS_SYM(Bi32_)))),
        VWT_Bi64_(readerWriter.getSymbolPointer(
            MANGLE_AS_STRING(VALUE_WITNESS_SYM(Bi64_)))),
        VWT_Bi128_(readerWriter.getSymbolPointer(
            MANGLE_AS_STRING(VALUE_WITNESS_SYM(Bi128_)))),
        VWT_Bi256_(readerWriter.getSymbolPointer(
            MANGLE_AS_STRING(VALUE_WITNESS_SYM(Bi256_)))),
        VWT_Bi512_(readerWriter.getSymbolPointer(
            MANGLE_AS_STRING(VALUE_WITNESS_SYM(Bi512_)))) {}

  void initializeValueMetadataFromPattern(
      WritableData<FullMetadata<TargetMetadata<Runtime>>> data,
      Size metadataOffset,
      Buffer<const TargetValueTypeDescriptor<Runtime>> descriptionBuffer,
      Buffer<const TargetGenericValueMetadataPattern<Runtime>> patternBuffer) {
    const auto *pattern = patternBuffer.ptr;

    char *metadataBase = reinterpret_cast<char *>(data.ptr);
    auto metadata =
        reinterpret_cast<ValueMetadata *>(metadataBase + metadataOffset);
    char *rawMetadata = reinterpret_cast<char *>(metadata);
    auto fullMetadata = asFullMetadata(metadata);

    if (pattern->hasExtraDataPattern()) {
      StoredPointer *metadataExtraData = reinterpret_cast<StoredPointer *>(
          rawMetadata + sizeof(TargetValueMetadata<Runtime>));
      auto extraDataPattern = pattern->getExtraDataPattern();

      // Zero memory up to the offset.
      // [pre-5.3-extra-data-zeroing] Before Swift 5.3, the runtime did not
      // correctly zero the zero-prefix of the extra-data pattern.
      memset(metadataExtraData, 0,
             size_t(extraDataPattern->OffsetInWords) * sizeof(StoredPointer));

      // Copy the pattern into the rest of the extra data.
      LOG("Writing %" PRIu16 "words of extra data from offset %" PRIu16,
          extraDataPattern->SizeInWords, extraDataPattern->OffsetInWords);
      auto patternPointers =
          patternBuffer.resolvePointer(&extraDataPattern->Pattern);
      for (unsigned i = 0; i < extraDataPattern->SizeInWords; i++) {
        auto patternPointer =
            patternPointers.resolvePointer(&patternPointers[i]);
        data.writePointer(
            &metadataExtraData[i + extraDataPattern->OffsetInWords],
            patternPointer);
      }
    }

    // Put the VWT pattern in place as if it was the real VWT.
    // The various initialization functions will instantiate this as
    // necessary.
    auto valueWitnesses =
        patternBuffer.resolvePointer(&pattern->ValueWitnesses);
    LOG("Setting initial value witnesses");
    data.writePointer(&fullMetadata->ValueWitnesses, valueWitnesses);

    // Set the metadata kind.
    LOG("Setting metadata kind %#x", (unsigned)pattern->getMetadataKind());
    metadata->setKind(pattern->getMetadataKind());

    // Set the type descriptor.
    LOG("Setting descriptor");
    data.writePointer(&metadata->Description, descriptionBuffer);
  }

  void installGenericArguments(
      WritableData<FullMetadata<TargetMetadata<Runtime>>> data,
      Size metadataOffset,
      Buffer<const TargetValueTypeDescriptor<Runtime>> descriptionBuffer,
      const GenericArgument *arguments) {
    LOG("Building %s", getDescriptorName(descriptionBuffer));
    char *metadataBase = reinterpret_cast<char *>(data.ptr);
    auto metadata =
        reinterpret_cast<ValueMetadata *>(metadataBase + metadataOffset);
    const auto &genericContext = *descriptionBuffer.ptr->getGenericContext();
    const auto &header = genericContext.getGenericContextHeader();
    auto dst = (reinterpret_cast<GenericArgument *>(metadata) +
                descriptionBuffer.ptr->getGenericArgumentOffset());
    LOG("Installing %" PRIu16 " generic arguments at offset %" PRId32,
        header.NumKeyArguments,
        descriptionBuffer.ptr->getGenericArgumentOffset());
    for (unsigned i = 0; i < header.NumKeyArguments; i++)
      data.writePointer(&dst[i], arguments[i]);

    // TODO: parameter pack support.
  }

  // Returns the constructed metadata, and the offset within the buffer to the
  // start of the ValueMetadata.
  std::pair<WritableData<FullMetadata<TargetMetadata<Runtime>>>, Size>
  buildGenericValueMetadata(
      Buffer<const TargetValueTypeDescriptor<Runtime>> descriptionBuffer,
      const GenericArgument *arguments,
      Buffer<const TargetGenericValueMetadataPattern<Runtime>> patternBuffer,
      size_t extraDataSize) {
    auto *pattern = patternBuffer.ptr;
    assert(!pattern->hasExtraDataPattern() ||
           (extraDataSize == (pattern->getExtraDataPattern()->OffsetInWords +
                              pattern->getExtraDataPattern()->SizeInWords) *
                                 sizeof(void *)));

    size_t totalSize =
        sizeof(FullMetadata<TargetValueMetadata<Runtime>>) + extraDataSize;
    LOG("Extra data size is %zu, allocating %zu bytes total", extraDataSize,
        totalSize);
    auto metadataBuffer =
        readerWriter.template allocate<FullMetadata<TargetMetadata<Runtime>>>(
            totalSize);
    auto metadataOffset = sizeof(ValueMetadata::HeaderType);

    initializeValueMetadataFromPattern(metadataBuffer, metadataOffset,
                                       descriptionBuffer, patternBuffer);

    // Copy the generic arguments into place.
    installGenericArguments(metadataBuffer, metadataOffset, descriptionBuffer,
                            arguments);

    return {metadataBuffer, metadataOffset};
  }

  void initializeGenericValueMetadata(
      WritableData<FullMetadata<TargetMetadata<Runtime>>> metadataBuffer) {
    auto *metadata = static_cast<TargetMetadata<Runtime> *>(metadataBuffer.ptr);
    auto *valueMetadata = dyn_cast<TargetValueMetadata<Runtime>>(metadata);

    auto descriptionBuffer =
        metadataBuffer.resolvePointer(&valueMetadata->Description);
    auto patternBuffer = descriptionBuffer.resolvePointer(
        &descriptionBuffer.ptr->getFullGenericContextHeader()
             .DefaultInstantiationPattern);
    auto completionFunction = patternBuffer.resolveFunctionPointer(
        &patternBuffer.ptr->CompletionFunction);

    if (!completionFunction) {
      LOG("Type has no completion function, skipping initialization");
      return;
    }

    if (auto structmd = dyn_cast<TargetStructMetadata<Runtime>>(metadata))
      initializeStructMetadata(metadataBuffer, structmd);
    else if (auto enummd = dyn_cast<TargetEnumMetadata<Runtime>>(metadata))
      initializeEnumMetadata(metadataBuffer, enummd);
    else
      LOG("Don't know how to initialize metadata kind %#" PRIx32,
          static_cast<uint32_t>(metadataBuffer.ptr->getKind()));
  }

  static constexpr TypeLayout getInitialLayoutForValueType() {
    return {0, 0, ValueWitnessFlags().withAlignment(1).withPOD(true), 0};
  }

  void copyVWT(WritableData<TargetValueWitnessTable<Runtime>> vwtBuffer,
               Buffer<const TargetValueWitnessTable<Runtime>> from) {
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID)                                      \
  auto LOWER_ID##_Buffer = from.resolveFunctionPointer(&from.ptr->LOWER_ID);   \
  vwtBuffer.writeFunctionPointer(&vwtBuffer.ptr->LOWER_ID, LOWER_ID##_Buffer);
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"
  }

  void installCommonValueWitnesses(
      const TypeLayout &layout,
      WritableData<TargetValueWitnessTable<Runtime>> vwtBuffer) {
    // TODO: expose the various symbols this needs, look them up, and write them
    // out.
    auto flags = layout.flags;
    if (flags.isPOD()) {
      // Use POD value witnesses.
      // If the value has a common size and alignment, use specialized value
      // witnesses we already have lying around for the builtin types.
      bool hasExtraInhabitants = layout.hasExtraInhabitants();
      LOG("type isPOD, hasExtraInhabitants=%s layout.size=%zu "
          "flags.getAlignmentMask=%zu",
          hasExtraInhabitants ? "true" : "false", (size_t)layout.size,
          (size_t)flags.getAlignmentMask());
      switch (sizeWithAlignmentMask(layout.size, flags.getAlignmentMask(),
                                    hasExtraInhabitants)) {
      default:
        // For uncommon layouts, use value witnesses that work with an arbitrary
        // size and alignment.
        LOG("Uncommon layout case, flags.isInlineStorage=%s",
            flags.isInlineStorage() ? "true" : "false");
        if (flags.isInlineStorage()) {
          vwtBuffer.writeFunctionPointer(
              &vwtBuffer.ptr->initializeBufferWithCopyOfBuffer,
              pod_direct_initializeBufferWithCopyOfBuffer);
        } else {
          vwtBuffer.writeFunctionPointer(
              &vwtBuffer.ptr->initializeBufferWithCopyOfBuffer,
              pod_indirect_initializeBufferWithCopyOfBuffer);
        }
        vwtBuffer.writeFunctionPointer(&vwtBuffer.ptr->destroy, pod_destroy);
        vwtBuffer.writeFunctionPointer(&vwtBuffer.ptr->initializeWithCopy,
                                       pod_copy);
        vwtBuffer.writeFunctionPointer(&vwtBuffer.ptr->initializeWithTake,
                                       pod_copy);
        vwtBuffer.writeFunctionPointer(&vwtBuffer.ptr->assignWithCopy,
                                       pod_copy);
        vwtBuffer.writeFunctionPointer(&vwtBuffer.ptr->assignWithTake,
                                       pod_copy);
        // getEnumTagSinglePayload and storeEnumTagSinglePayload are not
        // interestingly optimizable based on POD-ness.
        return;

      case sizeWithAlignmentMask(1, 0, 0):
        LOG("case sizeWithAlignmentMask(1, 0, 0)");
        copyVWT(vwtBuffer, VWT_Bi8_);
        break;
      case sizeWithAlignmentMask(2, 1, 0):
        LOG("case sizeWithAlignmentMask(2, 1, 0)");
        copyVWT(vwtBuffer, VWT_Bi16_);
        break;
      case sizeWithAlignmentMask(4, 3, 0):
        LOG("case sizeWithAlignmentMask(4, 3, 0)");
        copyVWT(vwtBuffer, VWT_Bi32_);
        break;
      case sizeWithAlignmentMask(8, 7, 0):
        LOG("case sizeWithAlignmentMask(8, 7, 0)");
        copyVWT(vwtBuffer, VWT_Bi64_);
        break;
      case sizeWithAlignmentMask(16, 15, 0):
        LOG("case sizeWithAlignmentMask(16, 15, 0)");
        copyVWT(vwtBuffer, VWT_Bi128_);
        break;
      case sizeWithAlignmentMask(32, 31, 0):
        LOG("case sizeWithAlignmentMask(32, 31, 0)");
        copyVWT(vwtBuffer, VWT_Bi256_);
        break;
      case sizeWithAlignmentMask(64, 63, 0):
        LOG("case sizeWithAlignmentMask(64, 63, 0)");
        copyVWT(vwtBuffer, VWT_Bi512_);
        break;
      }

      return;
    }

    if (flags.isBitwiseTakable()) {
      LOG("Is bitwise takable, setting pod_copy as initializeWithTake");
      // Use POD value witnesses for operations that do an initializeWithTake.
      vwtBuffer.writeFunctionPointer(&vwtBuffer.ptr->initializeWithTake,
                                     pod_copy);
      return;
    }
  }

  void initializeStructMetadata(
      WritableData<FullMetadata<TargetMetadata<Runtime>>> metadataBuffer,
      TargetStructMetadata<Runtime> *metadata) {
    LOG("Initializing struct");

    auto descriptionBuffer =
        metadataBuffer.resolvePointer(&metadata->Description);
    auto description =
        reinterpret_cast<const TargetStructDescriptor<Runtime> *>(
            descriptionBuffer.ptr);

    auto fieldDescriptorBuffer =
        descriptionBuffer.resolvePointer(&description->Fields);
    auto fieldDescriptor = fieldDescriptorBuffer.ptr;
    auto fields = fieldDescriptor->getFields();
    LOG("%zu fields", fields.size());

    auto layout = getInitialLayoutForValueType();
    size_t size = layout.size;
    size_t alignMask = layout.flags.getAlignmentMask();
    bool isPOD = layout.flags.isPOD();
    bool isBitwiseTakable = layout.flags.isBitwiseTakable();

    auto *fieldOffsetsStart = wordsOffset<StoredPointer>(
        metadata, description->FieldOffsetVectorOffset);
    auto *fieldOffsets = reinterpret_cast<uint32_t *>(fieldOffsetsStart);

    auto numGenericParams = description->getGenericContextHeader().NumParams;
    auto genericArguments =
        wordsOffset<ConstTargetMetadataPointer<Runtime, swift::TargetMetadata>>(
            metadata, description->getGenericArgumentOffset());

    // We have extra inhabitants if any element does. Use the field with the
    // most.
    unsigned extraInhabitantCount = 0;

    for (unsigned i = 0; i != fields.size(); ++i) {
      auto &field = fields[i];
      auto nameBuffer = fieldDescriptorBuffer.resolvePointer(&field.FieldName);
      auto mangledTypeNameBuffer =
          fieldDescriptorBuffer.resolvePointer(&field.MangledTypeName);
      auto mangledTypeName =
          Demangle::makeSymbolicMangledNameStringRef(mangledTypeNameBuffer.ptr);
      LOG("Examining field %u '%s' type '%.*s' (mangled name is %zu bytes)", i,
          nameBuffer.ptr, (int)mangledTypeName.size(), mangledTypeName.data(),
          mangledTypeName.size());

      // TODO: This needs to work out of process.
      SubstGenericParametersFromMetadata substitutions(metadata);
      auto result = swift_getTypeByMangledName(
          MetadataState::LayoutComplete, mangledTypeName,
          substitutions.getGenericArgs(),
          [&substitutions](unsigned depth, unsigned index) {
            auto result = substitutions.getMetadata(depth, index).Ptr;
            LOG("substitutions.getMetadata(%u, %u).Ptr = %p", depth, index,
                result);
            return result;
          },
          [&substitutions](const Metadata *type, unsigned index) {
            auto result = substitutions.getWitnessTable(type, index);
            LOG("substitutions.getWitnessTable(%p, %u) = %p", type, index,
                result);
            return result;
          });
      if (result.isError()) {
        auto *error = result.getError();
        char *errorStr = error->copyErrorString();
        LOG("Failed to look up field: %s", errorStr);
        error->freeErrorString(errorStr);
        abort(); // TODO: fail gracefully
      }
      auto *fieldType = result.getType().getMetadata();
      LOG("Looked up field type metadata %p", fieldType);

      // TODO: need to use resolvePointer on a buffer of some sort.
      auto *fieldWitnessTable = asFullMetadata(fieldType)->ValueWitnesses;
      auto *fieldLayout = fieldWitnessTable->getTypeLayout();
      size = roundUpToAlignMask(size, fieldLayout->flags.getAlignmentMask());

      fieldOffsets[i] = size;

      size += fieldLayout->size;
      alignMask = std::max(alignMask, fieldLayout->flags.getAlignmentMask());
      if (!fieldLayout->flags.isPOD())
        isPOD = false;
      if (!fieldLayout->flags.isBitwiseTakable())
        isBitwiseTakable = false;

      unsigned fieldExtraInhabitantCount =
          fieldWitnessTable->getNumExtraInhabitants();
      if (fieldExtraInhabitantCount > extraInhabitantCount) {
        extraInhabitantCount = fieldExtraInhabitantCount;
      }
    }

    bool isInline =
        ValueWitnessTable::isValueInline(isBitwiseTakable, size, alignMask + 1);

    layout.size = size;
    layout.flags = ValueWitnessFlags()
                       .withAlignmentMask(alignMask)
                       .withPOD(isPOD)
                       .withBitwiseTakable(isBitwiseTakable)
                       .withInlineStorage(isInline);
    layout.extraInhabitantCount = extraInhabitantCount;
    layout.stride = std::max(size_t(1), roundUpToAlignMask(size, alignMask));

    auto oldVWTBuffer = metadataBuffer.resolvePointer(
        &asFullMetadata(metadata)->ValueWitnesses);
    auto *oldVWT = oldVWTBuffer.ptr;

    if (LOG_ENABLED) {
      auto info = readerWriter.getSymbolInfo(oldVWTBuffer);
      LOG("Initializing new VWT from old VWT %#" PRIx64 " - %s (%s + %" PRIu64
          ")",
          oldVWTBuffer.getAddress(), info.symbolName.c_str(),
          info.libraryName.c_str(), info.pointerOffset);
    }

    auto newVWTData =
        readerWriter.template allocate<TargetValueWitnessTable<Runtime>>(
            sizeof(TargetValueWitnessTable<Runtime>));
    auto *newVWT = newVWTData.ptr;

    // Initialize the new table with the raw contents of the old table first.
    // This will set the data fields.
    new (newVWT) TargetValueWitnessTable<Runtime>(*oldVWT);

    // Set all the functions separately so they get the right fixups.
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)                           \
  // This macro intentionally left blank.
#define FUNCTION_VALUE_WITNESS(LOWER_ID, UPPER_ID, RETURN_TYPE, PARAM_TYPES)   \
  newVWTData.writeFunctionPointer(                                             \
      &newVWT->LOWER_ID,                                                       \
      oldVWTBuffer.resolveFunctionPointer(&oldVWT->LOWER_ID));
#include "swift/ABI/ValueWitness.def"

    installCommonValueWitnesses(layout, newVWTData);

    newVWT->publishLayout(layout);

    metadataBuffer.writePointer(&metadataBuffer.ptr->ValueWitnesses,
                                newVWTData);
  }

  void initializeEnumMetadata(
      WritableData<FullMetadata<TargetMetadata<Runtime>>> metadataBuffer,
      TargetEnumMetadata<Runtime> *metadata) {
    LOG("Initializing enum");
  }

  size_t extraDataSize(
      Buffer<const TargetTypeContextDescriptor<Runtime>> descriptionBuffer,
      Buffer<const TargetGenericMetadataPattern<Runtime>> patternBuffer) {
    LOG("Getting extra data size for %s", getDescriptorName(descriptionBuffer));

    auto *pattern = patternBuffer.ptr;

    auto *description = descriptionBuffer.ptr;

    if (auto *valueDescription = dyn_cast<ValueTypeDescriptor>(description)) {
      auto *valuePattern =
          reinterpret_cast<const TargetGenericValueMetadataPattern<Runtime> *>(
              pattern);
      if (valuePattern->hasExtraDataPattern()) {
        auto extraDataPattern = valuePattern->getExtraDataPattern();
        auto result =
            (extraDataPattern->OffsetInWords + extraDataPattern->SizeInWords) *
            sizeof(void *);
        LOG("Value type descriptor has extra data pattern, extra data size: "
            "%zu",
            result);
        return result;
      }

      if (auto structDescription = dyn_cast<StructDescriptor>(description)) {
        if (structDescription->hasFieldOffsetVector()) {
          auto fieldsStart =
              structDescription->FieldOffsetVectorOffset * sizeof(void *);
          auto fieldsEnd =
              fieldsStart + structDescription->NumFields * sizeof(uint32_t);
          auto size = fieldsEnd - sizeof(TargetStructMetadata<Runtime>);
          auto result = roundUpToAlignment(size, sizeof(StoredPointer));
          LOG("Struct descriptor has field offset vector, computed extra data "
              "size: %zu",
              result);
          return result;
        } else if (structDescription->isGeneric()) {
          const auto &genericContext = *structDescription->getGenericContext();
          const auto &header = genericContext.getGenericContextHeader();
          auto result = header.NumKeyArguments * sizeof(void *);
          LOG("Struct descriptor has no field offset vector, computed extra "
              "data size from generic arguments, extra data size: %zu",
              result);
          return result;
        }
      }

      if (auto enumDescription = dyn_cast<EnumDescriptor>(description)) {
        if (enumDescription->hasPayloadSizeOffset()) {
          auto offset = enumDescription->getPayloadSizeOffset();
          auto result = offset * sizeof(StoredPointer) -
                        sizeof(TargetEnumMetadata<Runtime>);
          LOG("Enum descriptor has payload size offset, computed extra data "
              "size: %zu",
              result);
          return result;
        } else if (enumDescription->isGeneric()) {
          const auto &genericContext = *enumDescription->getGenericContext();
          const auto &header = genericContext.getGenericContextHeader();
          auto result = header.NumKeyArguments * sizeof(void *);
          LOG("Enum descriptor has no payload size offset, computed extra data "
              "size from generic arguments, extra data size: %zu",
              result);
          return result;
        }
      }
    }

    abort();
  }

  template <typename Printer>
  class Dumper {
    Printer print;
    ReaderWriter readerWriter;

    template <typename T>
    void printPointer(Buffer<T> buffer) {
      auto info = readerWriter.getSymbolInfo(buffer);
      print("%#" PRIx64 " - %s (%s + %" PRIu64 ")", buffer.getAddress(),
            info.symbolName.c_str(), info.libraryName.c_str(),
            info.pointerOffset);
    }

    template <typename T>
    void printPointer(const char *prefix, Buffer<T> buffer,
                      const char *suffix = "\n") {
      print("%s", prefix);
      printPointer(buffer);
      print("%s", suffix);
    }

  public:
    Dumper(Printer print) : print(print) {}

    void dumpMetadata(Buffer<const TargetMetadata<Runtime>> metadataBuffer) {
      printPointer("Metadata ", metadataBuffer);

      auto fullMetadata = asFullMetadata(metadataBuffer.ptr);

      auto valueWitnesses =
          metadataBuffer.resolvePointer(&fullMetadata->ValueWitnesses);
      printPointer("  value witnesses: ", valueWitnesses);
      dumpVWT(valueWitnesses);

      auto kind = fullMetadata->getKind();
      auto kindString = getStringForMetadataKind(kind);
      print("  kind: %#" PRIx32 " (%s)\n", static_cast<uint32_t>(kind),
            kindString.str().c_str());

      if (auto classmd =
              dyn_cast<TargetClassMetadataType<Runtime>>(metadataBuffer.ptr))
        dumpClassMetadata(metadataBuffer, classmd);
      else if (auto valuemd =
                   dyn_cast<TargetValueMetadata<Runtime>>(metadataBuffer.ptr))
        dumpValueMetadata(metadataBuffer, valuemd);
    }

    void dumpClassMetadata(Buffer<const TargetMetadata<Runtime>> metadataBuffer,
                           const TargetClassMetadataType<Runtime> *metadata) {
      print("  TODO class dumping\n");
    }

    void dumpValueMetadata(Buffer<const TargetMetadata<Runtime>> metadataBuffer,
                           const TargetValueMetadata<Runtime> *metadata) {
      auto descriptionBuffer =
          metadataBuffer.resolvePointer(&metadata->Description);
      auto description = descriptionBuffer.ptr;
      printPointer("  description: ", descriptionBuffer);

      if (description->hasLayoutString()) {
        auto layoutStringBuffer = metadataBuffer.resolvePointer(
            &asFullMetadata(metadata)->layoutString);
        printPointer("  layout string: ", layoutStringBuffer);
      }

      auto name = descriptionBuffer.resolvePointer(&description->Name);
      printPointer("  name: ", name);
      print("        \"%s\"\n", name.ptr);

      if (auto structmd =
              dyn_cast<TargetStructMetadata<Runtime>>(metadataBuffer.ptr))
        dumpStructMetadata(metadataBuffer, structmd);
      else if (auto enummd =
                   dyn_cast<TargetEnumMetadata<Runtime>>(metadataBuffer.ptr))
        dumpEnumMetadata(metadataBuffer, enummd);

      if (description->isGeneric()) {
        auto numGenericParams =
            description->getGenericContextHeader().NumParams;
        auto genericArguments = wordsOffset<
            ConstTargetMetadataPointer<Runtime, swift::TargetMetadata>>(
            metadata, description->getGenericArgumentOffset());
        for (unsigned i = 0; i < numGenericParams; i++) {
          auto arg = metadataBuffer.resolvePointer(&genericArguments[i]);
          print("  genericArg[%u]: ", i);
          printPointer(arg);
          print("\n");
        }
      }
    }

    void
    dumpStructMetadata(Buffer<const TargetMetadata<Runtime>> metadataBuffer,
                       const TargetStructMetadata<Runtime> *metadata) {
      auto descriptionBuffer =
          metadataBuffer.resolvePointer(&metadata->Description);
      auto structDescription =
          reinterpret_cast<const TargetStructDescriptor<Runtime> *>(
              descriptionBuffer.ptr);
      if (structDescription->hasFieldOffsetVector()) {
        auto *offsetsStart = wordsOffset<StoredPointer>(
            metadata, structDescription->FieldOffsetVectorOffset);
        auto *offsets = reinterpret_cast<const uint32_t *>(offsetsStart);
        for (unsigned i = 0; i < structDescription->NumFields; i++)
          print("  fieldOffset[%u]: %" PRIu32 "\n", i, offsets[i]);
      }
    }

    void dumpEnumMetadata(Buffer<const TargetMetadata<Runtime>> metadataBuffer,
                          const TargetEnumMetadata<Runtime> *metadata) {
      auto descriptionBuffer =
          metadataBuffer.resolvePointer(&metadata->Description);
      auto description =
          reinterpret_cast<const TargetEnumDescriptor<Runtime> *>(
              descriptionBuffer.ptr);

      if (description->hasPayloadSizeOffset()) {
        auto payloadSizeOffset = description->getPayloadSizeOffset();
        print("  offset: %u\n", payloadSizeOffset);
        auto *payloadSizePtr =
            wordsOffset<StoredSize *>(metadata, payloadSizeOffset);
        print("  payload size: %" PRIu64 "\n", (uint64_t)*payloadSizePtr);
      }
    }

    void dumpVWT(Buffer<const TargetValueWitnessTable<Runtime>> vwtBuffer) {
      auto *vwt = vwtBuffer.ptr;

#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)                           \
  dumpVWTDataField(#LOWER_ID, vwt->LOWER_ID);
#define FUNCTION_VALUE_WITNESS(LOWER_ID, UPPER_ID, RETURN_TYPE, PARAM_TYPES)   \
  dumpVWTFunctionField(vwtBuffer, #LOWER_ID, &vwt->LOWER_ID);
#include "swift/ABI/ValueWitness.def"

      if (auto *enumVWT = dyn_cast<EnumValueWitnessTable>(vwt)) {
        // #define WANT_ONLY_ENUM_VALUE_WITNESSES
        // #define VALUE_WITNESS(LOWER_ID, UPPER_ID) \
//         dumpVWTField(vwtBuffer, #LOWER_ID, &enumVWT->LOWER_ID);
        // #include "swift/ABI/ValueWitness.def"
      }
    }

    void dumpVWTDataField(const char *name, uint64_t value) {
      print("    %s: %#" PRIx64 " (%" PRIu64 ")\n", name, value, value);
    }

    template <typename T>
    void dumpVWTDataField(const char *name, TargetValueWitnessFlags<T> value) {
      dumpVWTDataField(name, value.getOpaqueValue());
    }

    template <typename T>
    void dumpVWTFunctionField(
        Buffer<const TargetValueWitnessTable<Runtime>> vwtBuffer,
        const char *name, T *ptr) {
      auto function = vwtBuffer.resolveFunctionPointer(ptr);
      print("    %s: ", name);
      printPointer(function);
      print("\n");
    }
  };

  template <typename Printer>
  Dumper(Printer) -> Dumper<Printer>;
};

template <typename ReaderWriter>
GenericMetadataBuilder(ReaderWriter) -> GenericMetadataBuilder<ReaderWriter>;

// SWIFT_RUNTIME_EXPORT
ValueMetadata *swift_allocateGenericValueMetadata_new(
    const ValueTypeDescriptor *description, const void *arguments,
    const GenericValueMetadataPattern *pattern, size_t extraDataSize) {
  GenericMetadataBuilder builder{InProcessReaderWriter()};
  auto [data, offset] = builder.buildGenericValueMetadata(
      {description},
      reinterpret_cast<const InProcessReaderWriter::GenericArgument *>(
          arguments),
      {pattern}, extraDataSize);
  char *base = reinterpret_cast<char *>(data.ptr);
  return reinterpret_cast<ValueMetadata *>(base + offset);
}

void swift_initializeGenericValueMetadata(Metadata *metadata) {
  GenericMetadataBuilder builder{InProcessReaderWriter()};
  builder.initializeGenericValueMetadata({asFullMetadata(metadata), -1u});
}

size_t swift_genericValueDataExtraSize(const ValueTypeDescriptor *description,
                                       const GenericMetadataPattern *pattern) {
  GenericMetadataBuilder builder{InProcessReaderWriter()};
  return builder.extraDataSize({description}, {pattern});
}

void _swift_dumpMetadata(const Metadata *metadata) {
  GenericMetadataBuilder<InProcessReaderWriter>::Dumper dumper(printf);
  dumper.dumpMetadata({metadata});
}
