//===- SparseTensorRuntime.cpp - SparseTensor runtime support lib ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a light-weight runtime support library for
// manipulating sparse tensors from MLIR.  More specifically, it provides
// C-API wrappers so that MLIR-generated code can call into the C++ runtime
// support library.  The functionality provided in this library is meant
// to simplify benchmarking, testing, and debugging of MLIR code operating
// on sparse tensors.  However, the provided functionality is **not**
// part of core MLIR itself.
//
// The following memory-resident sparse storage schemes are supported:
//
// (a) A coordinate scheme for temporarily storing and lexicographically
//     sorting a sparse tensor by coordinate (SparseTensorCOO).
//
// (b) A "one-size-fits-all" sparse tensor storage scheme defined by
//     per-dimension sparse/dense annnotations together with a dimension
//     ordering used by MLIR compiler-generated code (SparseTensorStorage).
//
// The following external formats are supported:
//
// (1) Matrix Market Exchange (MME): *.mtx
//     https://math.nist.gov/MatrixMarket/formats.html
//
// (2) Formidable Repository of Open Sparse Tensors and Tools (FROSTT): *.tns
//     http://frostt.io/tensors/file-formats.html
//
// Two public APIs are supported:
//
// (I) Methods operating on MLIR buffers (memrefs) to interact with sparse
//     tensors. These methods should be used exclusively by MLIR
//     compiler-generated code.
//
// (II) Methods that accept C-style data structures to interact with sparse
//      tensors. These methods can be used by any external runtime that wants
//      to interact with MLIR compiler-generated code.
//
// In both cases (I) and (II), the SparseTensorStorage format is externally
// only visible as an opaque pointer.
//
//===----------------------------------------------------------------------===//

#include "mlir/ExecutionEngine/SparseTensorRuntime.h"

#ifdef MLIR_CRUNNERUTILS_DEFINE_FUNCTIONS

#include "mlir/ExecutionEngine/SparseTensor/ArithmeticUtils.h"
#include "mlir/ExecutionEngine/SparseTensor/COO.h"
#include "mlir/ExecutionEngine/SparseTensor/ErrorHandling.h"
#include "mlir/ExecutionEngine/SparseTensor/File.h"
#include "mlir/ExecutionEngine/SparseTensor/Storage.h"

#include <cstring>
#include <numeric>

using namespace mlir::sparse_tensor;

//===----------------------------------------------------------------------===//
//
// Implementation details for public functions, which don't have a good
// place to live in the C++ library this file is wrapping.
//
//===----------------------------------------------------------------------===//

namespace {

/// Wrapper class to avoid memory leakage issues.  The `SparseTensorCOO<V>`
/// class provides a standard C++ iterator interface, where the iterator
/// is implemented as per `std::vector`'s iterator.  However, for MLIR's
/// usage we need to have an iterator which also holds onto the underlying
/// `SparseTensorCOO<V>` so that it can be freed whenever the iterator
/// is freed.
//
// We name this `SparseTensorIterator` rather than `SparseTensorCOOIterator`
// for future-proofing, since the use of `SparseTensorCOO` is an
// implementation detail that we eventually want to change (e.g., to
// use `SparseTensorEnumerator` directly, rather than constructing the
// intermediate `SparseTensorCOO` at all).
template <typename V>
class SparseTensorIterator final {
public:
  /// This ctor requires `coo` to be a non-null pointer to a dynamically
  /// allocated object, and takes ownership of that object.  Therefore,
  /// callers must not free the underlying COO object, since the iterator's
  /// dtor will do so.
  explicit SparseTensorIterator(const SparseTensorCOO<V> *coo)
      : coo(coo), it(coo->begin()), end(coo->end()) {}

  ~SparseTensorIterator() { delete coo; }

  // Disable copy-ctor and copy-assignment, to prevent double-free.
  SparseTensorIterator(const SparseTensorIterator<V> &) = delete;
  SparseTensorIterator<V> &operator=(const SparseTensorIterator<V> &) = delete;

  /// Gets the next element.  If there are no remaining elements, then
  /// returns nullptr.
  const Element<V> *getNext() { return it < end ? &*it++ : nullptr; }

private:
  const SparseTensorCOO<V> *const coo; // Owning pointer.
  typename SparseTensorCOO<V>::const_iterator it;
  const typename SparseTensorCOO<V>::const_iterator end;
};

//===----------------------------------------------------------------------===//
//
// Utilities for manipulating `StridedMemRefType`.
//
//===----------------------------------------------------------------------===//

// We shouldn't need to use `detail::safelyEQ` here since the `1` is a literal.
#define ASSERT_NO_STRIDE(MEMREF)                                               \
  do {                                                                         \
    assert((MEMREF) && "Memref is nullptr");                                   \
    assert(((MEMREF)->strides[0] == 1) && "Memref has non-trivial stride");    \
  } while (false)

// All our functions use `uint64_t` for ranks, but `StridedMemRefType::sizes`
// uses `int64_t` on some platforms.  So we explicitly cast this lookup to
// ensure we get a consistent type, and we use `checkOverflowCast` rather
// than `static_cast` just to be extremely sure that the casting can't
// go awry.  (The cast should aways be safe since (1) sizes should never
// be negative, and (2) the maximum `int64_t` is smaller than the maximum
// `uint64_t`.  But it's better to be safe than sorry.)
#define MEMREF_GET_USIZE(MEMREF)                                               \
  detail::checkOverflowCast<uint64_t>((MEMREF)->sizes[0])

#define ASSERT_USIZE_EQ(MEMREF, SZ)                                            \
  assert(detail::safelyEQ(MEMREF_GET_USIZE(MEMREF), (SZ)) &&                   \
         "Memref size mismatch")

#define MEMREF_GET_PAYLOAD(MEMREF) ((MEMREF)->data + (MEMREF)->offset)

/// Initializes the memref with the provided size and data pointer.  This
/// is designed for functions which want to "return" a memref that aliases
/// into memory owned by some other object (e.g., `SparseTensorStorage`),
/// without doing any actual copying.  (The "return" is in scarequotes
/// because the `_mlir_ciface_` calling convention migrates any returned
/// memrefs into an out-parameter passed before all the other function
/// parameters.)
///
/// We make this a function rather than a macro mainly for type safety
/// reasons.  This function does not modify the data pointer, but it
/// cannot be marked `const` because it is stored into the (necessarily)
/// non-`const` memref.  This function is templated over the `DataSizeT`
/// to work around signedness warnings due to many data types having
/// varying signedness across different platforms.  The templating allows
/// this function to ensure that it does the right thing and never
/// introduces errors due to implicit conversions.
template <typename DataSizeT, typename T>
static inline void aliasIntoMemref(DataSizeT size, T *data,
                                   StridedMemRefType<T, 1> &ref) {
  ref.basePtr = ref.data = data;
  ref.offset = 0;
  using MemrefSizeT = typename std::remove_reference_t<decltype(ref.sizes[0])>;
  ref.sizes[0] = detail::checkOverflowCast<MemrefSizeT>(size);
  ref.strides[0] = 1;
}

} // anonymous namespace

extern "C" {

//===----------------------------------------------------------------------===//
//
// Public functions which operate on MLIR buffers (memrefs) to interact
// with sparse tensors (which are only visible as opaque pointers externally).
//
//===----------------------------------------------------------------------===//

#define CASE(p, c, v, P, C, V)                                                 \
  if (posTp == (p) && crdTp == (c) && valTp == (v)) {                          \
    switch (action) {                                                          \
    case Action::kEmpty:                                                       \
      return SparseTensorStorage<P, C, V>::newEmpty(                           \
          dimRank, dimSizes, lvlRank, lvlSizes, lvlTypes, lvl2dim);            \
    case Action::kFromCOO: {                                                   \
      assert(ptr && "Received nullptr for SparseTensorCOO object");            \
      auto &coo = *static_cast<SparseTensorCOO<V> *>(ptr);                     \
      return SparseTensorStorage<P, C, V>::newFromCOO(                         \
          dimRank, dimSizes, lvlRank, lvlTypes, lvl2dim, coo);                 \
    }                                                                          \
    case Action::kSparseToSparse: {                                            \
      assert(ptr && "Received nullptr for SparseTensorStorage object");        \
      auto &tensor = *static_cast<SparseTensorStorageBase *>(ptr);             \
      return SparseTensorStorage<P, C, V>::newFromSparseTensor(                \
          dimRank, dimSizes, lvlRank, lvlSizes, lvlTypes, lvl2dim, dimRank,    \
          dim2lvl, tensor);                                                    \
    }                                                                          \
    case Action::kEmptyCOO:                                                    \
      return new SparseTensorCOO<V>(lvlRank, lvlSizes);                        \
    case Action::kToCOO: {                                                     \
      assert(ptr && "Received nullptr for SparseTensorStorage object");        \
      auto &tensor = *static_cast<SparseTensorStorage<P, C, V> *>(ptr);        \
      return tensor.toCOO(lvlRank, lvlSizes, dimRank, dim2lvl);                \
    }                                                                          \
    case Action::kToIterator: {                                                \
      assert(ptr && "Received nullptr for SparseTensorStorage object");        \
      auto &tensor = *static_cast<SparseTensorStorage<P, C, V> *>(ptr);        \
      auto *coo = tensor.toCOO(lvlRank, lvlSizes, dimRank, dim2lvl);           \
      return new SparseTensorIterator<V>(coo);                                 \
    }                                                                          \
    case Action::kPack: {                                                      \
      assert(ptr && "Received nullptr for SparseTensorStorage object");        \
      intptr_t *buffers = static_cast<intptr_t *>(ptr);                        \
      return SparseTensorStorage<P, C, V>::packFromLvlBuffers(                 \
          dimRank, dimSizes, lvlRank, lvlSizes, lvlTypes, lvl2dim, dimRank,    \
          dim2lvl, buffers);                                                   \
    }                                                                          \
    }                                                                          \
    MLIR_SPARSETENSOR_FATAL("unknown action: %d\n",                            \
                            static_cast<uint32_t>(action));                    \
  }

#define CASE_SECSAME(p, v, P, V) CASE(p, p, v, P, P, V)

// Assume index_type is in fact uint64_t, so that _mlir_ciface_newSparseTensor
// can safely rewrite kIndex to kU64.  We make this assertion to guarantee
// that this file cannot get out of sync with its header.
static_assert(std::is_same<index_type, uint64_t>::value,
              "Expected index_type == uint64_t");

// The Swiss-army-knife for sparse tensor creation.
void *_mlir_ciface_newSparseTensor( // NOLINT
    StridedMemRefType<index_type, 1> *dimSizesRef,
    StridedMemRefType<index_type, 1> *lvlSizesRef,
    StridedMemRefType<DimLevelType, 1> *lvlTypesRef,
    StridedMemRefType<index_type, 1> *dim2lvlRef,
    StridedMemRefType<index_type, 1> *lvl2dimRef, OverheadType posTp,
    OverheadType crdTp, PrimaryType valTp, Action action, void *ptr) {
  ASSERT_NO_STRIDE(dimSizesRef);
  ASSERT_NO_STRIDE(lvlSizesRef);
  ASSERT_NO_STRIDE(lvlTypesRef);
  ASSERT_NO_STRIDE(dim2lvlRef);
  ASSERT_NO_STRIDE(lvl2dimRef);
  const uint64_t dimRank = MEMREF_GET_USIZE(dimSizesRef);
  const uint64_t lvlRank = MEMREF_GET_USIZE(lvlSizesRef);
  ASSERT_USIZE_EQ(lvlTypesRef, lvlRank);
  ASSERT_USIZE_EQ(dim2lvlRef, dimRank);
  ASSERT_USIZE_EQ(lvl2dimRef, lvlRank);
  const index_type *dimSizes = MEMREF_GET_PAYLOAD(dimSizesRef);
  const index_type *lvlSizes = MEMREF_GET_PAYLOAD(lvlSizesRef);
  const DimLevelType *lvlTypes = MEMREF_GET_PAYLOAD(lvlTypesRef);
  const index_type *dim2lvl = MEMREF_GET_PAYLOAD(dim2lvlRef);
  const index_type *lvl2dim = MEMREF_GET_PAYLOAD(lvl2dimRef);

  // Prepare map.
  // TODO: start using MapRef map(dimRank, lvlRank, dim2lvl, lvl2dim) below

  // Rewrite kIndex to kU64, to avoid introducing a bunch of new cases.
  // This is safe because of the static_assert above.
  if (posTp == OverheadType::kIndex)
    posTp = OverheadType::kU64;
  if (crdTp == OverheadType::kIndex)
    crdTp = OverheadType::kU64;

  // Double matrices with all combinations of overhead storage.
  CASE(OverheadType::kU64, OverheadType::kU64, PrimaryType::kF64, uint64_t,
       uint64_t, double);
  CASE(OverheadType::kU64, OverheadType::kU32, PrimaryType::kF64, uint64_t,
       uint32_t, double);
  CASE(OverheadType::kU64, OverheadType::kU16, PrimaryType::kF64, uint64_t,
       uint16_t, double);
  CASE(OverheadType::kU64, OverheadType::kU8, PrimaryType::kF64, uint64_t,
       uint8_t, double);
  CASE(OverheadType::kU32, OverheadType::kU64, PrimaryType::kF64, uint32_t,
       uint64_t, double);
  CASE(OverheadType::kU32, OverheadType::kU32, PrimaryType::kF64, uint32_t,
       uint32_t, double);
  CASE(OverheadType::kU32, OverheadType::kU16, PrimaryType::kF64, uint32_t,
       uint16_t, double);
  CASE(OverheadType::kU32, OverheadType::kU8, PrimaryType::kF64, uint32_t,
       uint8_t, double);
  CASE(OverheadType::kU16, OverheadType::kU64, PrimaryType::kF64, uint16_t,
       uint64_t, double);
  CASE(OverheadType::kU16, OverheadType::kU32, PrimaryType::kF64, uint16_t,
       uint32_t, double);
  CASE(OverheadType::kU16, OverheadType::kU16, PrimaryType::kF64, uint16_t,
       uint16_t, double);
  CASE(OverheadType::kU16, OverheadType::kU8, PrimaryType::kF64, uint16_t,
       uint8_t, double);
  CASE(OverheadType::kU8, OverheadType::kU64, PrimaryType::kF64, uint8_t,
       uint64_t, double);
  CASE(OverheadType::kU8, OverheadType::kU32, PrimaryType::kF64, uint8_t,
       uint32_t, double);
  CASE(OverheadType::kU8, OverheadType::kU16, PrimaryType::kF64, uint8_t,
       uint16_t, double);
  CASE(OverheadType::kU8, OverheadType::kU8, PrimaryType::kF64, uint8_t,
       uint8_t, double);

  // Float matrices with all combinations of overhead storage.
  CASE(OverheadType::kU64, OverheadType::kU64, PrimaryType::kF32, uint64_t,
       uint64_t, float);
  CASE(OverheadType::kU64, OverheadType::kU32, PrimaryType::kF32, uint64_t,
       uint32_t, float);
  CASE(OverheadType::kU64, OverheadType::kU16, PrimaryType::kF32, uint64_t,
       uint16_t, float);
  CASE(OverheadType::kU64, OverheadType::kU8, PrimaryType::kF32, uint64_t,
       uint8_t, float);
  CASE(OverheadType::kU32, OverheadType::kU64, PrimaryType::kF32, uint32_t,
       uint64_t, float);
  CASE(OverheadType::kU32, OverheadType::kU32, PrimaryType::kF32, uint32_t,
       uint32_t, float);
  CASE(OverheadType::kU32, OverheadType::kU16, PrimaryType::kF32, uint32_t,
       uint16_t, float);
  CASE(OverheadType::kU32, OverheadType::kU8, PrimaryType::kF32, uint32_t,
       uint8_t, float);
  CASE(OverheadType::kU16, OverheadType::kU64, PrimaryType::kF32, uint16_t,
       uint64_t, float);
  CASE(OverheadType::kU16, OverheadType::kU32, PrimaryType::kF32, uint16_t,
       uint32_t, float);
  CASE(OverheadType::kU16, OverheadType::kU16, PrimaryType::kF32, uint16_t,
       uint16_t, float);
  CASE(OverheadType::kU16, OverheadType::kU8, PrimaryType::kF32, uint16_t,
       uint8_t, float);
  CASE(OverheadType::kU8, OverheadType::kU64, PrimaryType::kF32, uint8_t,
       uint64_t, float);
  CASE(OverheadType::kU8, OverheadType::kU32, PrimaryType::kF32, uint8_t,
       uint32_t, float);
  CASE(OverheadType::kU8, OverheadType::kU16, PrimaryType::kF32, uint8_t,
       uint16_t, float);
  CASE(OverheadType::kU8, OverheadType::kU8, PrimaryType::kF32, uint8_t,
       uint8_t, float);

  // Two-byte floats with both overheads of the same type.
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kF16, uint64_t, f16);
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kBF16, uint64_t, bf16);
  CASE_SECSAME(OverheadType::kU32, PrimaryType::kF16, uint32_t, f16);
  CASE_SECSAME(OverheadType::kU32, PrimaryType::kBF16, uint32_t, bf16);
  CASE_SECSAME(OverheadType::kU16, PrimaryType::kF16, uint16_t, f16);
  CASE_SECSAME(OverheadType::kU16, PrimaryType::kBF16, uint16_t, bf16);
  CASE_SECSAME(OverheadType::kU8, PrimaryType::kF16, uint8_t, f16);
  CASE_SECSAME(OverheadType::kU8, PrimaryType::kBF16, uint8_t, bf16);

  // Integral matrices with both overheads of the same type.
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kI64, uint64_t, int64_t);
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kI32, uint64_t, int32_t);
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kI16, uint64_t, int16_t);
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kI8, uint64_t, int8_t);
  CASE_SECSAME(OverheadType::kU32, PrimaryType::kI64, uint32_t, int64_t);
  CASE_SECSAME(OverheadType::kU32, PrimaryType::kI32, uint32_t, int32_t);
  CASE_SECSAME(OverheadType::kU32, PrimaryType::kI16, uint32_t, int16_t);
  CASE_SECSAME(OverheadType::kU32, PrimaryType::kI8, uint32_t, int8_t);
  CASE_SECSAME(OverheadType::kU16, PrimaryType::kI64, uint16_t, int64_t);
  CASE_SECSAME(OverheadType::kU16, PrimaryType::kI32, uint16_t, int32_t);
  CASE_SECSAME(OverheadType::kU16, PrimaryType::kI16, uint16_t, int16_t);
  CASE_SECSAME(OverheadType::kU16, PrimaryType::kI8, uint16_t, int8_t);
  CASE_SECSAME(OverheadType::kU8, PrimaryType::kI64, uint8_t, int64_t);
  CASE_SECSAME(OverheadType::kU8, PrimaryType::kI32, uint8_t, int32_t);
  CASE_SECSAME(OverheadType::kU8, PrimaryType::kI16, uint8_t, int16_t);
  CASE_SECSAME(OverheadType::kU8, PrimaryType::kI8, uint8_t, int8_t);

  // Complex matrices with wide overhead.
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kC64, uint64_t, complex64);
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kC32, uint64_t, complex32);

  // Unsupported case (add above if needed).
  // TODO: better pretty-printing of enum values!
  MLIR_SPARSETENSOR_FATAL(
      "unsupported combination of types: <P=%d, C=%d, V=%d>\n",
      static_cast<int>(posTp), static_cast<int>(crdTp),
      static_cast<int>(valTp));
}
#undef CASE
#undef CASE_SECSAME

#define IMPL_SPARSEVALUES(VNAME, V)                                            \
  void _mlir_ciface_sparseValues##VNAME(StridedMemRefType<V, 1> *ref,          \
                                        void *tensor) {                        \
    assert(ref &&tensor);                                                      \
    std::vector<V> *v;                                                         \
    static_cast<SparseTensorStorageBase *>(tensor)->getValues(&v);             \
    assert(v);                                                                 \
    aliasIntoMemref(v->size(), v->data(), *ref);                               \
  }
MLIR_SPARSETENSOR_FOREVERY_V(IMPL_SPARSEVALUES)
#undef IMPL_SPARSEVALUES

#define IMPL_GETOVERHEAD(NAME, TYPE, LIB)                                      \
  void _mlir_ciface_##NAME(StridedMemRefType<TYPE, 1> *ref, void *tensor,      \
                           index_type lvl) {                                   \
    assert(ref &&tensor);                                                      \
    std::vector<TYPE> *v;                                                      \
    static_cast<SparseTensorStorageBase *>(tensor)->LIB(&v, lvl);              \
    assert(v);                                                                 \
    aliasIntoMemref(v->size(), v->data(), *ref);                               \
  }
#define IMPL_SPARSEPOSITIONS(PNAME, P)                                         \
  IMPL_GETOVERHEAD(sparsePositions##PNAME, P, getPositions)
MLIR_SPARSETENSOR_FOREVERY_O(IMPL_SPARSEPOSITIONS)
#undef IMPL_SPARSEPOSITIONS

#define IMPL_SPARSECOORDINATES(CNAME, C)                                       \
  IMPL_GETOVERHEAD(sparseCoordinates##CNAME, C, getCoordinates)
MLIR_SPARSETENSOR_FOREVERY_O(IMPL_SPARSECOORDINATES)
#undef IMPL_SPARSECOORDINATES
#undef IMPL_GETOVERHEAD

// TODO: use MapRef here for translation of coordinates
// TOOD: remove dim2lvl
#define IMPL_ADDELT(VNAME, V)                                                  \
  void *_mlir_ciface_addElt##VNAME(                                            \
      void *lvlCOO, StridedMemRefType<V, 0> *vref,                             \
      StridedMemRefType<index_type, 1> *dimCoordsRef,                          \
      StridedMemRefType<index_type, 1> *dim2lvlRef) {                          \
    assert(lvlCOO &&vref);                                                     \
    ASSERT_NO_STRIDE(dimCoordsRef);                                            \
    ASSERT_NO_STRIDE(dim2lvlRef);                                              \
    const uint64_t rank = MEMREF_GET_USIZE(dimCoordsRef);                      \
    ASSERT_USIZE_EQ(dim2lvlRef, rank);                                         \
    const index_type *dimCoords = MEMREF_GET_PAYLOAD(dimCoordsRef);            \
    const index_type *dim2lvl = MEMREF_GET_PAYLOAD(dim2lvlRef);                \
    std::vector<index_type> lvlCoords(rank);                                   \
    for (uint64_t d = 0; d < rank; ++d)                                        \
      lvlCoords[dim2lvl[d]] = dimCoords[d];                                    \
    V *value = MEMREF_GET_PAYLOAD(vref);                                       \
    static_cast<SparseTensorCOO<V> *>(lvlCOO)->add(lvlCoords, *value);         \
    return lvlCOO;                                                             \
  }
MLIR_SPARSETENSOR_FOREVERY_V(IMPL_ADDELT)
#undef IMPL_ADDELT

// NOTE: the `cref` argument uses the same coordinate-space as the `iter`
// (which can be either dim- or lvl-coords, depending on context).
#define IMPL_GETNEXT(VNAME, V)                                                 \
  bool _mlir_ciface_getNext##VNAME(void *iter,                                 \
                                   StridedMemRefType<index_type, 1> *cref,     \
                                   StridedMemRefType<V, 0> *vref) {            \
    assert(iter &&vref);                                                       \
    ASSERT_NO_STRIDE(cref);                                                    \
    index_type *coords = MEMREF_GET_PAYLOAD(cref);                             \
    V *value = MEMREF_GET_PAYLOAD(vref);                                       \
    const uint64_t rank = MEMREF_GET_USIZE(cref);                              \
    const Element<V> *elem =                                                   \
        static_cast<SparseTensorIterator<V> *>(iter)->getNext();               \
    if (elem == nullptr)                                                       \
      return false;                                                            \
    for (uint64_t d = 0; d < rank; d++)                                        \
      coords[d] = elem->coords[d];                                             \
    *value = elem->value;                                                      \
    return true;                                                               \
  }
MLIR_SPARSETENSOR_FOREVERY_V(IMPL_GETNEXT)
#undef IMPL_GETNEXT

#define IMPL_LEXINSERT(VNAME, V)                                               \
  void _mlir_ciface_lexInsert##VNAME(                                          \
      void *t, StridedMemRefType<index_type, 1> *lvlCoordsRef,                 \
      StridedMemRefType<V, 0> *vref) {                                         \
    assert(t &&vref);                                                          \
    auto &tensor = *static_cast<SparseTensorStorageBase *>(t);                 \
    ASSERT_NO_STRIDE(lvlCoordsRef);                                            \
    index_type *lvlCoords = MEMREF_GET_PAYLOAD(lvlCoordsRef);                  \
    assert(lvlCoords);                                                         \
    V *value = MEMREF_GET_PAYLOAD(vref);                                       \
    tensor.lexInsert(lvlCoords, *value);                                       \
  }
MLIR_SPARSETENSOR_FOREVERY_V(IMPL_LEXINSERT)
#undef IMPL_LEXINSERT

#define IMPL_EXPINSERT(VNAME, V)                                               \
  void _mlir_ciface_expInsert##VNAME(                                          \
      void *t, StridedMemRefType<index_type, 1> *lvlCoordsRef,                 \
      StridedMemRefType<V, 1> *vref, StridedMemRefType<bool, 1> *fref,         \
      StridedMemRefType<index_type, 1> *aref, index_type count) {              \
    assert(t);                                                                 \
    auto &tensor = *static_cast<SparseTensorStorageBase *>(t);                 \
    ASSERT_NO_STRIDE(lvlCoordsRef);                                            \
    ASSERT_NO_STRIDE(vref);                                                    \
    ASSERT_NO_STRIDE(fref);                                                    \
    ASSERT_NO_STRIDE(aref);                                                    \
    ASSERT_USIZE_EQ(vref, MEMREF_GET_USIZE(fref));                             \
    index_type *lvlCoords = MEMREF_GET_PAYLOAD(lvlCoordsRef);                  \
    V *values = MEMREF_GET_PAYLOAD(vref);                                      \
    bool *filled = MEMREF_GET_PAYLOAD(fref);                                   \
    index_type *added = MEMREF_GET_PAYLOAD(aref);                              \
    tensor.expInsert(lvlCoords, values, filled, added, count);                 \
  }
MLIR_SPARSETENSOR_FOREVERY_V(IMPL_EXPINSERT)
#undef IMPL_EXPINSERT

void *_mlir_ciface_createCheckedSparseTensorReader(
    char *filename, StridedMemRefType<index_type, 1> *dimShapeRef,
    PrimaryType valTp) {
  ASSERT_NO_STRIDE(dimShapeRef);
  const uint64_t dimRank = MEMREF_GET_USIZE(dimShapeRef);
  const index_type *dimShape = MEMREF_GET_PAYLOAD(dimShapeRef);
  auto *reader = SparseTensorReader::create(filename, dimRank, dimShape, valTp);
  return static_cast<void *>(reader);
}

void _mlir_ciface_getSparseTensorReaderDimSizes(
    StridedMemRefType<index_type, 1> *out, void *p) {
  assert(out && p);
  SparseTensorReader &reader = *static_cast<SparseTensorReader *>(p);
  auto *dimSizes = const_cast<uint64_t *>(reader.getDimSizes());
  aliasIntoMemref(reader.getRank(), dimSizes, *out);
}

#define IMPL_GETNEXT(VNAME, V, CNAME, C)                                       \
  bool _mlir_ciface_getSparseTensorReaderReadToBuffers##CNAME##VNAME(          \
      void *p, StridedMemRefType<index_type, 1> *dim2lvlRef,                   \
      StridedMemRefType<index_type, 1> *lvl2dimRef,                            \
      StridedMemRefType<C, 1> *cref, StridedMemRefType<V, 1> *vref) {          \
    assert(p);                                                                 \
    auto &reader = *static_cast<SparseTensorReader *>(p);                      \
    ASSERT_NO_STRIDE(dim2lvlRef);                                              \
    ASSERT_NO_STRIDE(lvl2dimRef);                                              \
    ASSERT_NO_STRIDE(cref);                                                    \
    ASSERT_NO_STRIDE(vref);                                                    \
    const uint64_t dimRank = reader.getRank();                                 \
    const uint64_t lvlRank = MEMREF_GET_USIZE(lvl2dimRef);                     \
    const uint64_t cSize = MEMREF_GET_USIZE(cref);                             \
    const uint64_t vSize = MEMREF_GET_USIZE(vref);                             \
    ASSERT_USIZE_EQ(dim2lvlRef, dimRank);                                      \
    assert(cSize >= lvlRank * vSize);                                          \
    assert(vSize >= reader.getNSE() && "Not enough space in buffers");         \
    (void)dimRank;                                                             \
    (void)cSize;                                                               \
    (void)vSize;                                                               \
    index_type *dim2lvl = MEMREF_GET_PAYLOAD(dim2lvlRef);                      \
    index_type *lvl2dim = MEMREF_GET_PAYLOAD(lvl2dimRef);                      \
    C *lvlCoordinates = MEMREF_GET_PAYLOAD(cref);                              \
    V *values = MEMREF_GET_PAYLOAD(vref);                                      \
    return reader.readToBuffers<C, V>(lvlRank, dim2lvl, lvl2dim,               \
                                      lvlCoordinates, values);                 \
  }
MLIR_SPARSETENSOR_FOREVERY_V_O(IMPL_GETNEXT)
#undef IMPL_GETNEXT

void *_mlir_ciface_newSparseTensorFromReader(
    void *p, StridedMemRefType<index_type, 1> *lvlSizesRef,
    StridedMemRefType<DimLevelType, 1> *lvlTypesRef,
    StridedMemRefType<index_type, 1> *dim2lvlRef,
    StridedMemRefType<index_type, 1> *lvl2dimRef, OverheadType posTp,
    OverheadType crdTp, PrimaryType valTp) {
  assert(p);
  SparseTensorReader &reader = *static_cast<SparseTensorReader *>(p);
  ASSERT_NO_STRIDE(lvlSizesRef);
  ASSERT_NO_STRIDE(lvlTypesRef);
  ASSERT_NO_STRIDE(dim2lvlRef);
  ASSERT_NO_STRIDE(lvl2dimRef);
  const uint64_t dimRank = reader.getRank();
  const uint64_t lvlRank = MEMREF_GET_USIZE(lvlSizesRef);
  ASSERT_USIZE_EQ(lvlTypesRef, lvlRank);
  ASSERT_USIZE_EQ(dim2lvlRef, dimRank);
  ASSERT_USIZE_EQ(lvl2dimRef, lvlRank);
  (void)dimRank;
  const index_type *lvlSizes = MEMREF_GET_PAYLOAD(lvlSizesRef);
  const DimLevelType *lvlTypes = MEMREF_GET_PAYLOAD(lvlTypesRef);
  const index_type *dim2lvl = MEMREF_GET_PAYLOAD(dim2lvlRef);
  const index_type *lvl2dim = MEMREF_GET_PAYLOAD(lvl2dimRef);
#define CASE(p, c, v, P, C, V)                                                 \
  if (posTp == OverheadType::p && crdTp == OverheadType::c &&                  \
      valTp == PrimaryType::v)                                                 \
    return static_cast<void *>(reader.readSparseTensor<P, C, V>(               \
        lvlRank, lvlSizes, lvlTypes, dim2lvl, lvl2dim));
#define CASE_SECSAME(p, v, P, V) CASE(p, p, v, P, P, V)
  // Rewrite kIndex to kU64, to avoid introducing a bunch of new cases.
  // This is safe because of the static_assert above.
  if (posTp == OverheadType::kIndex)
    posTp = OverheadType::kU64;
  if (crdTp == OverheadType::kIndex)
    crdTp = OverheadType::kU64;
  // Double matrices with all combinations of overhead storage.
  CASE(kU64, kU64, kF64, uint64_t, uint64_t, double);
  CASE(kU64, kU32, kF64, uint64_t, uint32_t, double);
  CASE(kU64, kU16, kF64, uint64_t, uint16_t, double);
  CASE(kU64, kU8, kF64, uint64_t, uint8_t, double);
  CASE(kU32, kU64, kF64, uint32_t, uint64_t, double);
  CASE(kU32, kU32, kF64, uint32_t, uint32_t, double);
  CASE(kU32, kU16, kF64, uint32_t, uint16_t, double);
  CASE(kU32, kU8, kF64, uint32_t, uint8_t, double);
  CASE(kU16, kU64, kF64, uint16_t, uint64_t, double);
  CASE(kU16, kU32, kF64, uint16_t, uint32_t, double);
  CASE(kU16, kU16, kF64, uint16_t, uint16_t, double);
  CASE(kU16, kU8, kF64, uint16_t, uint8_t, double);
  CASE(kU8, kU64, kF64, uint8_t, uint64_t, double);
  CASE(kU8, kU32, kF64, uint8_t, uint32_t, double);
  CASE(kU8, kU16, kF64, uint8_t, uint16_t, double);
  CASE(kU8, kU8, kF64, uint8_t, uint8_t, double);
  // Float matrices with all combinations of overhead storage.
  CASE(kU64, kU64, kF32, uint64_t, uint64_t, float);
  CASE(kU64, kU32, kF32, uint64_t, uint32_t, float);
  CASE(kU64, kU16, kF32, uint64_t, uint16_t, float);
  CASE(kU64, kU8, kF32, uint64_t, uint8_t, float);
  CASE(kU32, kU64, kF32, uint32_t, uint64_t, float);
  CASE(kU32, kU32, kF32, uint32_t, uint32_t, float);
  CASE(kU32, kU16, kF32, uint32_t, uint16_t, float);
  CASE(kU32, kU8, kF32, uint32_t, uint8_t, float);
  CASE(kU16, kU64, kF32, uint16_t, uint64_t, float);
  CASE(kU16, kU32, kF32, uint16_t, uint32_t, float);
  CASE(kU16, kU16, kF32, uint16_t, uint16_t, float);
  CASE(kU16, kU8, kF32, uint16_t, uint8_t, float);
  CASE(kU8, kU64, kF32, uint8_t, uint64_t, float);
  CASE(kU8, kU32, kF32, uint8_t, uint32_t, float);
  CASE(kU8, kU16, kF32, uint8_t, uint16_t, float);
  CASE(kU8, kU8, kF32, uint8_t, uint8_t, float);
  // Two-byte floats with both overheads of the same type.
  CASE_SECSAME(kU64, kF16, uint64_t, f16);
  CASE_SECSAME(kU64, kBF16, uint64_t, bf16);
  CASE_SECSAME(kU32, kF16, uint32_t, f16);
  CASE_SECSAME(kU32, kBF16, uint32_t, bf16);
  CASE_SECSAME(kU16, kF16, uint16_t, f16);
  CASE_SECSAME(kU16, kBF16, uint16_t, bf16);
  CASE_SECSAME(kU8, kF16, uint8_t, f16);
  CASE_SECSAME(kU8, kBF16, uint8_t, bf16);
  // Integral matrices with both overheads of the same type.
  CASE_SECSAME(kU64, kI64, uint64_t, int64_t);
  CASE_SECSAME(kU64, kI32, uint64_t, int32_t);
  CASE_SECSAME(kU64, kI16, uint64_t, int16_t);
  CASE_SECSAME(kU64, kI8, uint64_t, int8_t);
  CASE_SECSAME(kU32, kI64, uint32_t, int64_t);
  CASE_SECSAME(kU32, kI32, uint32_t, int32_t);
  CASE_SECSAME(kU32, kI16, uint32_t, int16_t);
  CASE_SECSAME(kU32, kI8, uint32_t, int8_t);
  CASE_SECSAME(kU16, kI64, uint16_t, int64_t);
  CASE_SECSAME(kU16, kI32, uint16_t, int32_t);
  CASE_SECSAME(kU16, kI16, uint16_t, int16_t);
  CASE_SECSAME(kU16, kI8, uint16_t, int8_t);
  CASE_SECSAME(kU8, kI64, uint8_t, int64_t);
  CASE_SECSAME(kU8, kI32, uint8_t, int32_t);
  CASE_SECSAME(kU8, kI16, uint8_t, int16_t);
  CASE_SECSAME(kU8, kI8, uint8_t, int8_t);
  // Complex matrices with wide overhead.
  CASE_SECSAME(kU64, kC64, uint64_t, complex64);
  CASE_SECSAME(kU64, kC32, uint64_t, complex32);

  // Unsupported case (add above if needed).
  // TODO: better pretty-printing of enum values!
  MLIR_SPARSETENSOR_FATAL(
      "unsupported combination of types: <P=%d, C=%d, V=%d>\n",
      static_cast<int>(posTp), static_cast<int>(crdTp),
      static_cast<int>(valTp));
#undef CASE_SECSAME
#undef CASE
}

void _mlir_ciface_outSparseTensorWriterMetaData(
    void *p, index_type dimRank, index_type nse,
    StridedMemRefType<index_type, 1> *dimSizesRef) {
  assert(p);
  ASSERT_NO_STRIDE(dimSizesRef);
  assert(dimRank != 0);
  index_type *dimSizes = MEMREF_GET_PAYLOAD(dimSizesRef);
  SparseTensorWriter &file = *static_cast<SparseTensorWriter *>(p);
  file << dimRank << " " << nse << std::endl;
  for (index_type d = 0; d < dimRank - 1; ++d)
    file << dimSizes[d] << " ";
  file << dimSizes[dimRank - 1] << std::endl;
}

#define IMPL_OUTNEXT(VNAME, V)                                                 \
  void _mlir_ciface_outSparseTensorWriterNext##VNAME(                          \
      void *p, index_type dimRank,                                             \
      StridedMemRefType<index_type, 1> *dimCoordsRef,                          \
      StridedMemRefType<V, 0> *vref) {                                         \
    assert(p &&vref);                                                          \
    ASSERT_NO_STRIDE(dimCoordsRef);                                            \
    const index_type *dimCoords = MEMREF_GET_PAYLOAD(dimCoordsRef);            \
    SparseTensorWriter &file = *static_cast<SparseTensorWriter *>(p);          \
    for (index_type d = 0; d < dimRank; ++d)                                   \
      file << (dimCoords[d] + 1) << " ";                                       \
    V *value = MEMREF_GET_PAYLOAD(vref);                                       \
    file << *value << std::endl;                                               \
  }
MLIR_SPARSETENSOR_FOREVERY_V(IMPL_OUTNEXT)
#undef IMPL_OUTNEXT

//===----------------------------------------------------------------------===//
//
// Public functions which accept only C-style data structures to interact
// with sparse tensors (which are only visible as opaque pointers externally).
//
//===----------------------------------------------------------------------===//

index_type sparseLvlSize(void *tensor, index_type l) {
  return static_cast<SparseTensorStorageBase *>(tensor)->getLvlSize(l);
}

index_type sparseDimSize(void *tensor, index_type d) {
  return static_cast<SparseTensorStorageBase *>(tensor)->getDimSize(d);
}

void endInsert(void *tensor) {
  return static_cast<SparseTensorStorageBase *>(tensor)->endInsert();
}

#define IMPL_OUTSPARSETENSOR(VNAME, V)                                         \
  void outSparseTensor##VNAME(void *coo, void *dest, bool sort) {              \
    assert(coo && "Got nullptr for COO object");                               \
    auto &coo_ = *static_cast<SparseTensorCOO<V> *>(coo);                      \
    if (sort)                                                                  \
      coo_.sort();                                                             \
    return writeExtFROSTT(coo_, static_cast<char *>(dest));                    \
  }
MLIR_SPARSETENSOR_FOREVERY_V(IMPL_OUTSPARSETENSOR)
#undef IMPL_OUTSPARSETENSOR

void delSparseTensor(void *tensor) {
  delete static_cast<SparseTensorStorageBase *>(tensor);
}

#define IMPL_DELCOO(VNAME, V)                                                  \
  void delSparseTensorCOO##VNAME(void *coo) {                                  \
    delete static_cast<SparseTensorCOO<V> *>(coo);                             \
  }
MLIR_SPARSETENSOR_FOREVERY_V(IMPL_DELCOO)
#undef IMPL_DELCOO

#define IMPL_DELITER(VNAME, V)                                                 \
  void delSparseTensorIterator##VNAME(void *iter) {                            \
    delete static_cast<SparseTensorIterator<V> *>(iter);                       \
  }
MLIR_SPARSETENSOR_FOREVERY_V(IMPL_DELITER)
#undef IMPL_DELITER

char *getTensorFilename(index_type id) {
  constexpr size_t BUF_SIZE = 80;
  char var[BUF_SIZE];
  snprintf(var, BUF_SIZE, "TENSOR%" PRIu64, id);
  char *env = getenv(var);
  if (!env)
    MLIR_SPARSETENSOR_FATAL("Environment variable %s is not set\n", var);
  return env;
}

void readSparseTensorShape(char *filename, std::vector<uint64_t> *out) {
  assert(out && "Received nullptr for out-parameter");
  SparseTensorReader reader(filename);
  reader.openFile();
  reader.readHeader();
  reader.closeFile();
  const uint64_t dimRank = reader.getRank();
  const uint64_t *dimSizes = reader.getDimSizes();
  out->reserve(dimRank);
  out->assign(dimSizes, dimSizes + dimRank);
}

index_type getSparseTensorReaderRank(void *p) {
  return static_cast<SparseTensorReader *>(p)->getRank();
}

bool getSparseTensorReaderIsSymmetric(void *p) {
  return static_cast<SparseTensorReader *>(p)->isSymmetric();
}

index_type getSparseTensorReaderNSE(void *p) {
  return static_cast<SparseTensorReader *>(p)->getNSE();
}

index_type getSparseTensorReaderDimSize(void *p, index_type d) {
  return static_cast<SparseTensorReader *>(p)->getDimSize(d);
}

void delSparseTensorReader(void *p) {
  delete static_cast<SparseTensorReader *>(p);
}

void *createSparseTensorWriter(char *filename) {
  SparseTensorWriter *file =
      (filename[0] == 0) ? &std::cout : new std::ofstream(filename);
  *file << "# extended FROSTT format\n";
  return static_cast<void *>(file);
}

void delSparseTensorWriter(void *p) {
  SparseTensorWriter *file = static_cast<SparseTensorWriter *>(p);
  file->flush();
  assert(file->good());
  if (file != &std::cout)
    delete file;
}

} // extern "C"

#undef MEMREF_GET_PAYLOAD
#undef ASSERT_USIZE_EQ
#undef MEMREF_GET_USIZE
#undef ASSERT_NO_STRIDE

#endif // MLIR_CRUNNERUTILS_DEFINE_FUNCTIONS
