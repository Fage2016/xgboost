/**
 *  Copyright 2019-2025, XGBoost Contributors
 * \file adapter.h
 */
#ifndef XGBOOST_DATA_ADAPTER_H_
#define XGBOOST_DATA_ADAPTER_H_
#include <dmlc/data.h>

#include <algorithm>  // for transform, all_of
#include <cstddef>    // for size_t
#include <cstdint>    // for uint8_t
#include <limits>     // for numeric_limits
#include <memory>     // for unique_ptr, make_unique
#include <utility>    // for move
#include <variant>    // for variant
#include <vector>     // for vector

#include "../data/cat_container.h"  // for CatAccessor
#include "array_interface.h"        // for ArrayInterface
#include "entry.h"                  // for COOTuple
#include "xgboost/base.h"
#include "xgboost/data.h"
#include "xgboost/logging.h"
#include "xgboost/span.h"
#include "xgboost/string_view.h"

namespace xgboost::data {
/**  External data formats should implement an adapter as below. The
 * adapter provides a uniform access to data outside xgboost, allowing
 * construction of DMatrix objects from a range of sources without duplicating
 * code.
 *
 * The adapter object is an iterator that returns batches of data. Each batch
 * contains a number of "lines". A line represents a set of elements from a
 * sparse input matrix, normally a row in the case of a CSR matrix or a column
 * for a CSC matrix. Typically in sparse matrix formats we can efficiently
 * access subsets of elements at a time, but cannot efficiently lookups elements
 * by random access, hence the "line" abstraction, allowing the sparse matrix to
 * return subsets of elements efficiently. Individual elements are described by
 * a COO tuple (row index, column index, value).
 *
 * This abstraction allows us to read through different sparse matrix formats
 * using the same interface. In particular we can write a DMatrix constructor
 * that uses the same code to construct itself from a CSR matrix, CSC matrix,
 * dense matrix, CSV, LIBSVM file, or potentially other formats. To see why this
 * is necessary, imagine we have 5 external matrix formats and 5 internal
 * DMatrix types where each DMatrix needs a custom constructor for each possible
 * input. The number of constructors is 5*5=25. Using an abstraction over the
 * input data types the number of constructors is reduced to 5, as each DMatrix
 * is oblivious to the external data format. Adding a new input source is simply
 * a case of implementing an adapter.
 *
 * Most of the below adapters do not need more than one batch as the data
 * originates from an in memory source. The file adapter does require batches to
 * avoid loading the entire file in memory.
 *
 * An important detail is empty row/column handling. Files loaded from disk do
 * not provide meta information about the number of rows/columns to expect, this
 * needs to be inferred during construction. Other sparse formats may specify a
 * number of rows/columns, but we can encounter entirely sparse rows or columns,
 * leading to disagreement between the inferred number and the meta-info
 * provided. To resolve this, adapters have methods specifying the number of
 * rows/columns expected, these methods may return zero where these values must
 * be inferred from data. A constructed DMatrix should agree with the input
 * source on numbers of rows/columns, appending empty rows if necessary.
 *  */

/** \brief An adapter can return this value for number of rows or columns
 * indicating that this value is currently unknown and should be inferred while
 * passing over the data. */
constexpr size_t kAdapterUnknownSize = std::numeric_limits<size_t >::max();

namespace detail {

/**
 * \brief Simplifies the use of DataIter when there is only one batch.
 */
template <typename DType>
class SingleBatchDataIter : dmlc::DataIter<DType> {
 public:
  void BeforeFirst() override { counter_ = 0; }
  bool Next() override {
    if (counter_ == 0) {
      counter_++;
      return true;
    }
    return false;
  }

 private:
  int counter_{0};
};

/** \brief Indicates this data source cannot contain meta-info such as labels,
 * weights or qid. */
class NoMetaInfo {
 public:
  const float* Labels() const { return nullptr; }
  const float* Weights() const { return nullptr; }
  const uint64_t* Qid() const { return nullptr; }
  const float* BaseMargin() const { return nullptr; }
};
};  // namespace detail

class DenseAdapterBatch : public detail::NoMetaInfo {
 public:
  DenseAdapterBatch(const float* values, bst_idx_t num_rows, bst_idx_t num_features)
      : values_(values), num_rows_(num_rows), num_features_(num_features) {}

 private:
  class Line {
   public:
    Line(const float* values, size_t size, size_t row_idx)
        : row_idx_(row_idx), size_(size), values_(values) {}

    size_t Size() const { return size_; }
    COOTuple GetElement(size_t idx) const {
      return COOTuple{row_idx_, idx, values_[idx]};
    }

   private:
    size_t row_idx_;
    size_t size_;
    const float* values_;
  };

 public:
  size_t Size() const { return num_rows_; }
  const Line GetLine(size_t idx) const {
    return Line(values_ + idx * num_features_, num_features_, idx);
  }
  static constexpr bool kIsRowMajor = true;

 private:
  const float* values_;
  size_t num_rows_;
  size_t num_features_;
};

class DenseAdapter : public detail::SingleBatchDataIter<DenseAdapterBatch> {
 public:
  DenseAdapter(const float* values, size_t num_rows, size_t num_features)
      : batch_(values, num_rows, num_features),
        num_rows_(num_rows),
        num_columns_(num_features) {}
  const DenseAdapterBatch& Value() const override { return batch_; }

  size_t NumRows() const { return num_rows_; }
  size_t NumColumns() const { return num_columns_; }

 private:
  DenseAdapterBatch batch_;
  size_t num_rows_;
  size_t num_columns_;
};

class ArrayAdapterBatch : public detail::NoMetaInfo {
 public:
  static constexpr bool kIsRowMajor = true;

 private:
  ArrayInterface<2> array_interface_;

  class Line {
    ArrayInterface<2> array_interface_;
    size_t ridx_;

   public:
    Line(ArrayInterface<2> array_interface, size_t ridx)
        : array_interface_{std::move(array_interface)}, ridx_{ridx} {}

    size_t Size() const { return array_interface_.Shape<1>(); }

    COOTuple GetElement(size_t idx) const {
      return {ridx_, idx, array_interface_(ridx_, idx)};
    }
  };

 public:
  ArrayAdapterBatch() = default;
  Line const GetLine(size_t idx) const {
    return Line{array_interface_, idx};
  }

  [[nodiscard]] std::size_t NumRows() const { return array_interface_.Shape<0>(); }
  [[nodiscard]] std::size_t NumCols() const { return array_interface_.Shape<1>(); }
  [[nodiscard]] std::size_t Size() const { return this->NumRows(); }

  explicit ArrayAdapterBatch(ArrayInterface<2> array_interface)
      : array_interface_{std::move(array_interface)} {}
};

/**
 * Adapter for dense array on host, in Python that's `numpy.ndarray`.  This is similar to
 * `DenseAdapter`, but supports __array_interface__ instead of raw pointers.  An
 * advantage is this can handle various data type without making a copy.
 */
class ArrayAdapter : public detail::SingleBatchDataIter<ArrayAdapterBatch> {
 public:
  explicit ArrayAdapter(StringView array_interface) {
    auto j = Json::Load(array_interface);
    array_interface_ = ArrayInterface<2>(get<Object const>(j));
    batch_ = ArrayAdapterBatch{array_interface_};
  }
  [[nodiscard]] ArrayAdapterBatch const& Value() const override { return batch_; }
  [[nodiscard]] std::size_t NumRows() const { return array_interface_.Shape<0>(); }
  [[nodiscard]] std::size_t NumColumns() const { return array_interface_.Shape<1>(); }

 private:
  ArrayAdapterBatch batch_;
  ArrayInterface<2> array_interface_;
};

class CSRArrayAdapterBatch : public detail::NoMetaInfo {
  ArrayInterface<1> indptr_;
  ArrayInterface<1> indices_;
  ArrayInterface<1> values_;
  bst_feature_t n_features_;

  class Line {
    ArrayInterface<1> indices_;
    ArrayInterface<1> values_;
    size_t ridx_;
    size_t offset_;

   public:
    Line(ArrayInterface<1> indices, ArrayInterface<1> values, size_t ridx,
         size_t offset)
        : indices_{std::move(indices)}, values_{std::move(values)}, ridx_{ridx},
          offset_{offset} {}

    [[nodiscard]] COOTuple GetElement(std::size_t idx) const {
      return {ridx_, TypedIndex<std::size_t, 1>{indices_}(offset_ + idx), values_(offset_ + idx)};
    }

    [[nodiscard]] std::size_t Size() const {
      return values_.Shape<0>();
    }
  };

 public:
  static constexpr bool kIsRowMajor = true;

 public:
  CSRArrayAdapterBatch() = default;
  CSRArrayAdapterBatch(ArrayInterface<1> indptr, ArrayInterface<1> indices,
                       ArrayInterface<1> values, bst_feature_t n_features)
      : indptr_{std::move(indptr)},
        indices_{std::move(indices)},
        values_{std::move(values)},
        n_features_{n_features} {
  }

  [[nodiscard]] std::size_t NumRows() const {
    size_t size = indptr_.Shape<0>();
    size = size == 0 ? 0 : size - 1;
    return size;
  }
  [[nodiscard]] std::size_t NumCols() const { return n_features_; }
  [[nodiscard]] std::size_t Size() const { return this->NumRows(); }

  [[nodiscard]] Line const GetLine(size_t idx) const {
    auto begin_no_stride = TypedIndex<size_t, 1>{indptr_}(idx);
    auto end_no_stride = TypedIndex<size_t, 1>{indptr_}(idx + 1);

    auto indices = indices_;
    auto values = values_;
    // Slice indices and values, stride remains unchanged since this is slicing by
    // specific index.
    auto offset = indices.strides[0] * begin_no_stride;

    indices.shape[0] = end_no_stride - begin_no_stride;
    values.shape[0] = end_no_stride - begin_no_stride;

    return Line{indices, values, idx, offset};
  }
};

/**
 * @brief Adapter for CSR array on host, in Python that's `scipy.sparse.csr_matrix`.
 */
class CSRArrayAdapter : public detail::SingleBatchDataIter<CSRArrayAdapterBatch> {
 public:
  CSRArrayAdapter(StringView indptr, StringView indices, StringView values,
                  size_t num_cols)
      : indptr_{indptr}, indices_{indices}, values_{values}, num_cols_{num_cols} {
    batch_ = CSRArrayAdapterBatch{indptr_, indices_, values_,
                                  static_cast<bst_feature_t>(num_cols_)};
  }

  [[nodiscard]] CSRArrayAdapterBatch const& Value() const override { return batch_; }
  [[nodiscard]] std::size_t NumRows() const {
    size_t size = indptr_.Shape<0>();
    size = size == 0 ? 0 : size - 1;
    return size;
  }
  [[nodiscard]] std::size_t NumColumns() const { return num_cols_; }

 private:
  CSRArrayAdapterBatch batch_;
  ArrayInterface<1> indptr_;
  ArrayInterface<1> indices_;
  ArrayInterface<1> values_;
  size_t num_cols_;
};

class CSCArrayAdapterBatch : public detail::NoMetaInfo {
  ArrayInterface<1> indptr_;
  ArrayInterface<1> indices_;
  ArrayInterface<1> values_;

  class Line {
    std::size_t column_idx_;
    ArrayInterface<1> row_idx_;
    ArrayInterface<1> values_;
    std::size_t offset_;

   public:
    Line(std::size_t idx, ArrayInterface<1> row_idx, ArrayInterface<1> values, std::size_t offset)
        : column_idx_{idx},
          row_idx_{std::move(row_idx)},
          values_{std::move(values)},
          offset_{offset} {}

    [[nodiscard]] std::size_t Size() const { return values_.Shape<0>(); }
    [[nodiscard]] COOTuple GetElement(std::size_t idx) const {
      return {TypedIndex<std::size_t, 1>{row_idx_}(offset_ + idx), column_idx_,
              values_(offset_ + idx)};
    }
  };

 public:
  static constexpr bool kIsRowMajor = false;

  CSCArrayAdapterBatch(ArrayInterface<1> indptr, ArrayInterface<1> indices,
                       ArrayInterface<1> values)
      : indptr_{std::move(indptr)}, indices_{std::move(indices)}, values_{std::move(values)} {}

  [[nodiscard]] std::size_t Size() const noexcept(true) {
    auto n = indptr_.n;
    return (n == 0) ? n : (n - 1);
  }
  [[nodiscard]] Line GetLine(std::size_t idx) const {
    auto begin_no_stride = TypedIndex<std::size_t, 1>{indptr_}(idx);
    auto end_no_stride = TypedIndex<std::size_t, 1>{indptr_}(idx + 1);

    auto indices = indices_;
    auto values = values_;
    // Slice indices and values, stride remains unchanged since this is slicing by
    // specific index.
    auto offset = indices.strides[0] * begin_no_stride;
    indices.shape[0] = end_no_stride - begin_no_stride;
    values.shape[0] = end_no_stride - begin_no_stride;

    return Line{idx, indices, values, offset};
  }
};

/**
 * @brief CSC adapter with support for array interface.
 */
class CSCArrayAdapter : public detail::SingleBatchDataIter<CSCArrayAdapterBatch> {
  ArrayInterface<1> indptr_;
  ArrayInterface<1> indices_;
  ArrayInterface<1> values_;
  size_t num_rows_;
  CSCArrayAdapterBatch batch_;

 public:
  CSCArrayAdapter(StringView indptr, StringView indices, StringView values, std::size_t num_rows)
      : indptr_{indptr},
        indices_{indices},
        values_{values},
        num_rows_{num_rows},
        batch_{CSCArrayAdapterBatch{indptr_, indices_, values_}} {}

  // JVM package sends 0 as unknown
  [[nodiscard]] std::size_t NumRows() const {
    return num_rows_ == 0 ? kAdapterUnknownSize : num_rows_;
  }
  [[nodiscard]] std::size_t NumColumns() const { return indptr_.n - 1; }
  [[nodiscard]] const CSCArrayAdapterBatch& Value() const override { return batch_; }
};

template <typename EncAccessor>
class EncColumnarAdapterBatchImpl : public detail::NoMetaInfo {
  using ArrayInf = std::add_const_t<ArrayInterface<1>>;

  common::Span<ArrayInf> columns_;
  EncAccessor acc_;

  class Line {
    common::Span<ArrayInf> const& columns_;
    std::size_t const ridx_;
    EncAccessor const& acc_;

   public:
    explicit Line(common::Span<ArrayInf> const& columns, EncAccessor const& acc, std::size_t ridx)
        : columns_{columns}, ridx_{ridx}, acc_{acc} {}
    [[nodiscard]] std::size_t Size() const { return columns_.empty() ? 0 : columns_.size(); }

    [[nodiscard]] COOTuple GetElement(std::size_t fidx) const {
      auto const& column = columns_.data()[fidx];
      float value = column.valid.Data() == nullptr || column.valid.Check(ridx_)
                        ? column(ridx_)
                        : std::numeric_limits<float>::quiet_NaN();
      return {ridx_, fidx, acc_(value, fidx)};
    }
  };

 public:
  EncColumnarAdapterBatchImpl() = default;
  explicit EncColumnarAdapterBatchImpl(common::Span<ArrayInf> columns, EncAccessor acc)
      : columns_{columns}, acc_{std::move(acc)} {}
  [[nodiscard]] Line GetLine(std::size_t ridx) const { return Line{columns_, this->acc_, ridx}; }
  [[nodiscard]] std::size_t Size() const {
    return columns_.empty() ? 0 : columns_.front().template Shape<0>();
  }
  [[nodiscard]] std::size_t NumCols() const { return columns_.empty() ? 0 : columns_.size(); }
  [[nodiscard]] std::size_t NumRows() const { return this->Size(); }

  static constexpr bool kIsRowMajor = true;
};

using ColumnarAdapterBatch = EncColumnarAdapterBatchImpl<NoOpAccessor>;
using EncColumnarAdapterBatch = EncColumnarAdapterBatchImpl<CatAccessor>;

/**
 * @brief Adapter for columnar format (arrow).
 *
 *   Supports both numeric values and categorical values.
 *
 * See @ref XGDMatrixCreateFromColumnar for notes
 */
class ColumnarAdapter : public detail::SingleBatchDataIter<ColumnarAdapterBatch> {
  std::vector<ArrayInterface<1>> columns_;
  enc::HostColumnsView ref_cats_;
  std::vector<enc::HostCatIndexView> cats_;
  std::vector<std::int32_t> cat_segments_;
  ColumnarAdapterBatch batch_;
  std::size_t n_bytes_{0};

  [[nodiscard]] static bool HasCatImpl(std::vector<enc::HostCatIndexView> const& cats) {
    return !std::all_of(cats.cbegin(), cats.cend(), [](auto const& cats) {
      return std::visit([](auto&& cats) { return cats.empty(); }, cats);
    });
  }

 public:
  /**
   * @brief JSON-encoded array of columns.
   */
  explicit ColumnarAdapter(StringView columns);

  [[nodiscard]] ColumnarAdapterBatch const& Value() const override { return batch_; }

  [[nodiscard]] bst_idx_t NumRows() const {
    if (!columns_.empty()) {
      return columns_.front().shape[0];
    }
    return 0;
  }
  [[nodiscard]] bst_idx_t NumColumns() const { return columns_.size(); }

  [[nodiscard]] bool HasCategorical() const { return HasCatImpl(this->cats_); }
  [[nodiscard]] bool HasRefCategorical() const { return !this->ref_cats_.Empty(); }

  [[nodiscard]] std::size_t SizeBytes() const { return n_bytes_; }

  [[nodiscard]] enc::HostColumnsView Cats() const {
    return {this->cats_, this->cat_segments_,
            static_cast<std::int32_t>(this->cat_segments_.back())};
  }
  [[nodiscard]] enc::HostColumnsView RefCats() const { return this->ref_cats_; }
  [[nodiscard]] common::Span<ArrayInterface<1> const> Columns() const { return this->columns_; }
};

inline auto MakeEncColumnarBatch(Context const* ctx, ColumnarAdapter const* adapter) {
  auto cats = std::make_unique<CatContainer>(adapter->RefCats());
  cats->Sort(ctx);
  auto [acc, mapping] = cpu_impl::MakeCatAccessor(ctx, adapter->Cats(), cats.get());
  return std::tuple{EncColumnarAdapterBatch{adapter->Columns(), acc}, std::move(mapping)};
}

class FileAdapterBatch {
 public:
  class Line {
   public:
    Line(size_t row_idx, const uint32_t *feature_idx, const float *value,
         size_t size)
        : row_idx_(row_idx),
          feature_idx_(feature_idx),
          value_(value),
          size_(size) {}

    size_t Size() { return size_; }
    COOTuple GetElement(size_t idx) {
      float fvalue = value_ == nullptr ? 1.0f : value_[idx];
      return COOTuple{row_idx_, feature_idx_[idx], fvalue};
    }

   private:
    size_t row_idx_;
    const uint32_t* feature_idx_;
    const float* value_;
    size_t size_;
  };
  FileAdapterBatch(const dmlc::RowBlock<uint32_t>* block, size_t row_offset)
      : block_(block), row_offset_(row_offset) {}
  Line GetLine(size_t idx) const {
    auto begin = block_->offset[idx];
    auto end = block_->offset[idx + 1];
    return Line{idx + row_offset_, &block_->index[begin], &block_->value[begin],
                end - begin};
  }
  const float* Labels() const { return block_->label; }
  const float* Weights() const { return block_->weight; }
  const uint64_t* Qid() const { return block_->qid; }
  const float* BaseMargin() const { return nullptr; }

  size_t Size() const { return block_->size; }
  static constexpr bool kIsRowMajor = true;

 private:
  const dmlc::RowBlock<uint32_t>* block_;
  size_t row_offset_;
};

/** \brief FileAdapter wraps dmlc::parser to read files and provide access in a
 * common interface. */
class FileAdapter : dmlc::DataIter<FileAdapterBatch> {
 public:
  explicit FileAdapter(dmlc::Parser<uint32_t>* parser) : parser_(parser) {}

  const FileAdapterBatch& Value() const override { return *batch_.get(); }
  void BeforeFirst() override {
    batch_.reset();
    parser_->BeforeFirst();
    row_offset_ = 0;
  }
  bool Next() override {
    bool next = parser_->Next();
    batch_.reset(new FileAdapterBatch(&parser_->Value(), row_offset_));
    row_offset_ += parser_->Value().size;
    return next;
  }
  // Indicates a number of rows/columns must be inferred
  size_t NumRows() const { return kAdapterUnknownSize; }
  size_t NumColumns() const { return kAdapterUnknownSize; }

 private:
  size_t row_offset_{0};
  std::unique_ptr<FileAdapterBatch> batch_;
  dmlc::Parser<uint32_t>* parser_;
};

/**
 * @brief Data iterator that takes callback to return data, used in JVM package for accepting data
 *        iterator.
 */
template <typename DataIterHandle, typename XGBCallbackDataIterNext, typename XGBoostBatchCSR>
class IteratorAdapter : public dmlc::DataIter<FileAdapterBatch> {
 public:
  IteratorAdapter(DataIterHandle data_handle, XGBCallbackDataIterNext* next_callback)
      : columns_{data::kAdapterUnknownSize},
        data_handle_(data_handle),
        next_callback_(next_callback) {}

  // override functions
  void BeforeFirst() override {
    CHECK(at_first_) << "Cannot reset IteratorAdapter";
  }

  [[nodiscard]] bool Next() override;

  [[nodiscard]] FileAdapterBatch const& Value() const override {
    return *batch_.get();
  }

  // callback to set the data
  void SetData(const XGBoostBatchCSR& batch) {
    offset_.clear();
    label_.clear();
    weight_.clear();
    index_.clear();
    value_.clear();
    offset_.insert(offset_.end(), batch.offset, batch.offset + batch.size + 1);

    if (batch.label != nullptr) {
      label_.insert(label_.end(), batch.label, batch.label + batch.size);
    }
    if (batch.weight != nullptr) {
      weight_.insert(weight_.end(), batch.weight, batch.weight + batch.size);
    }
    if (batch.index != nullptr) {
      index_.insert(index_.end(), batch.index + offset_[0],
                    batch.index + offset_.back());
    }
    if (batch.value != nullptr) {
      value_.insert(value_.end(), batch.value + offset_[0],
                    batch.value + offset_.back());
    }
    if (offset_[0] != 0) {
      size_t base = offset_[0];
      for (size_t &item : offset_) {
        item -= base;
      }
    }
    CHECK(columns_ == data::kAdapterUnknownSize || columns_ == batch.columns)
        << "Number of columns between batches changed from " << columns_
        << " to " << batch.columns;

    columns_ = batch.columns;
    block_.size = batch.size;

    block_.offset = dmlc::BeginPtr(offset_);
    block_.label = dmlc::BeginPtr(label_);
    block_.weight = dmlc::BeginPtr(weight_);
    block_.qid = nullptr;
    block_.field = nullptr;
    block_.index = dmlc::BeginPtr(index_);
    block_.value = dmlc::BeginPtr(value_);

    batch_ = std::make_unique<FileAdapterBatch>(&block_, row_offset_);
    row_offset_ += offset_.size() - 1;
  }

  [[nodiscard]] std::size_t NumColumns() const { return columns_; }
  [[nodiscard]] std::size_t NumRows() const { return kAdapterUnknownSize; }

 private:
  std::vector<size_t> offset_;
  std::vector<dmlc::real_t> label_;
  std::vector<dmlc::real_t> weight_;
  std::vector<uint32_t> index_;
  std::vector<dmlc::real_t> value_;

  size_t columns_;
  size_t row_offset_{0};
  // at the beginning.
  bool at_first_{true};
  // handle to the iterator,
  DataIterHandle data_handle_;
  // call back to get the data.
  XGBCallbackDataIterNext *next_callback_;
  // internal Rowblock
  dmlc::RowBlock<uint32_t> block_;
  std::unique_ptr<FileAdapterBatch> batch_;
};

class SparsePageAdapterBatch {
  HostSparsePageView page_;

 public:
  struct Line {
    Entry const* inst;
    size_t n;
    bst_idx_t ridx;
    COOTuple GetElement(size_t idx) const { return {ridx, inst[idx].index, inst[idx].fvalue}; }
    size_t Size() const { return n; }
  };

  explicit SparsePageAdapterBatch(HostSparsePageView page) : page_{std::move(page)} {}
  Line GetLine(size_t ridx) const { return Line{page_[ridx].data(), page_[ridx].size(), ridx}; }
  size_t Size() const { return page_.Size(); }
};
}  // namespace xgboost::data
#endif  // XGBOOST_DATA_ADAPTER_H_
