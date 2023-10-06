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
#include "swift/ABI/Metadata.h"
#include "swift/ABI/TargetLayout.h"
#include "swift/Runtime/Metadata.h"

using namespace swift;

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

  class WritableData {
  public:
    WritableData(void *ptr, size_t size) : ptr(ptr), size(size) {}

    void *ptr;
    size_t size;
    size_t cursor;

    template <typename T>
    void writePointer(StoredPointer *to, Buffer<T> target) {
      *to = reinterpret_cast<StoredPointer>(target.ptr);
    }

    template <typename T>
    void writePointer(T **to, Buffer<T> target) {
      *to = target.ptr;
    }

    void writePointer(GenericArgument *to, GenericArgument target) {
      *to = target;
    }
  };

  template <typename T>
  class Buffer {
  public:
    T *ptr;

    T &operator[](size_t i) { return ptr[i]; }

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
  };

  WritableData allocate(size_t size);
  template <typename T>
  T *read(ReadableData &data, size_t offset);
  void write();
};

InProcessReaderWriter::WritableData
InProcessReaderWriter::allocate(size_t size) {
  auto bytes =
      (char *)MetadataAllocator(MetadataAllocatorTags::GenericValueMetadataTag)
          .Allocate(size, alignof(void *));

  return WritableData{bytes, size};
}

template <typename ReaderWriter>
class GenericMetadataBuilder {
  ReaderWriter readerWriter;

  using Runtime = typename ReaderWriter::Runtime;

  template <typename T>
  using Pointer = typename ReaderWriter::Runtime::template Pointer<T>;

  template <typename T>
  using Buffer = typename ReaderWriter::template Buffer<T>;

  using Size = typename ReaderWriter::Size;

  using WritableData = typename ReaderWriter::WritableData;

  using StoredPointer = typename ReaderWriter::Runtime::StoredPointer;

  using GenericArgument = typename ReaderWriter::GenericArgument;

public:
  void initializeValueMetadataFromPattern(
      WritableData data, Size metadataOffset,
      Buffer<const TargetValueTypeDescriptor<Runtime>> descriptionBuffer,
      Buffer<const TargetGenericValueMetadataPattern<Runtime>> patternBuffer) {
    const auto *pattern = patternBuffer.ptr;

    char *metadataBase = static_cast<char *>(data.ptr);
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
      //       copyMetadataPattern(metadataExtraData, extraDataPattern);

      auto patternPointers =
          patternBuffer.resolvePointer(&extraDataPattern->Pattern);
      for (unsigned i = 0; i < extraDataPattern->SizeInWords; i++) {
        auto patternPointer =
            patternPointers.resolvePointer(&patternPointers[i]);
        data.writePointer(
            &metadataExtraData[i + extraDataPattern->OffsetInWords],
            patternPointer);
      }
      //       memcpy(section + pattern->OffsetInWords,
      //              pattern->Pattern.get(),
      //              size_t(pattern->SizeInWords) * sizeof(void*));
    }

    // Put the VWT pattern in place as if it was the real VWT.
    // The various initialization functions will instantiate this as
    // necessary.
    auto valueWitnesses =
        patternBuffer.resolvePointer(&pattern->ValueWitnesses);
    data.writePointer(&fullMetadata->ValueWitnesses, valueWitnesses);

    // Set the metadata kind.
    metadata->setKind(pattern->getMetadataKind());

    // Set the type descriptor.
    data.writePointer(&metadata->Description, descriptionBuffer);
  }

  void installGenericArguments(
      WritableData data, Size metadataOffset,
      Buffer<const TargetValueTypeDescriptor<Runtime>> descriptionBuffer,
      const GenericArgument *arguments) {
    char *metadataBase = static_cast<char *>(data.ptr);
    auto metadata =
        reinterpret_cast<ValueMetadata *>(metadataBase + metadataOffset);
    const auto &genericContext = *descriptionBuffer.ptr->getGenericContext();
    const auto &header = genericContext.getGenericContextHeader();
    auto dst = (reinterpret_cast<GenericArgument *>(metadata) +
                descriptionBuffer.ptr->getGenericArgumentOffset());
    for (unsigned i = 0; i < header.NumKeyArguments; i++)
      data.writePointer(&dst[i], arguments[i]);

    // TODO: parameter pack support.
  }

  // Returns the constructed metadata, and the offset within the buffer to the
  // start of the ValueMetadata.
  std::pair<WritableData, Size> buildGenericValueMetadata(
      Buffer<const TargetValueTypeDescriptor<Runtime>> descriptionBuffer,
      const GenericArgument *arguments,
      Buffer<const TargetGenericValueMetadataPattern<Runtime>> patternBuffer,
      size_t extraDataSize) {
    auto *pattern = patternBuffer.ptr;
    assert(!pattern->hasExtraDataPattern() ||
           (extraDataSize == (pattern->getExtraDataPattern()->OffsetInWords +
                              pattern->getExtraDataPattern()->SizeInWords) *
                                 sizeof(void *)));

    size_t totalSize = sizeof(FullMetadata<ValueMetadata>) + extraDataSize;
    auto metadataBuffer = readerWriter.allocate(totalSize);
    auto metadataOffset = sizeof(ValueMetadata::HeaderType);

    initializeValueMetadataFromPattern(metadataBuffer, metadataOffset,
                                       descriptionBuffer, patternBuffer);

    // Copy the generic arguments into place.
    installGenericArguments(metadataBuffer, metadataOffset, descriptionBuffer,
                            arguments);

    return {metadataBuffer, metadataOffset};
  }
};

// SWIFT_RUNTIME_EXPORT
ValueMetadata *swift_allocateGenericValueMetadata_new(
    const ValueTypeDescriptor *description, const void *arguments,
    const GenericValueMetadataPattern *pattern, size_t extraDataSize) {
  GenericMetadataBuilder<InProcessReaderWriter> builder;
  auto [data, offset] = builder.buildGenericValueMetadata(
      {description},
      reinterpret_cast<const InProcessReaderWriter::GenericArgument *>(
          arguments),
      {pattern}, extraDataSize);
  char *base = static_cast<char *>(data.ptr);
  return reinterpret_cast<ValueMetadata *>(base + offset);
}
