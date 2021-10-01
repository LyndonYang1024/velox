/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/type/Filter.h"

namespace facebook::velox::common {

std::string Filter::toString() const {
  const char* strKind = "<unknown>";
  switch (kind_) {
    case FilterKind::kAlwaysFalse:
      strKind = "AlwaysFalse";
      break;
    case FilterKind::kAlwaysTrue:
      strKind = "AlwaysTrue";
      break;
    case FilterKind::kIsNull:
      strKind = "IsNull";
      break;
    case FilterKind::kIsNotNull:
      strKind = "IsNotNull";
      break;
    case FilterKind::kBoolValue:
      strKind = "BoolValue";
      break;
    case FilterKind::kBigintRange:
      strKind = "BigintRange";
      break;
    case FilterKind::kBigintValuesUsingHashTable:
      strKind = "BigintValuesUsingHashTable";
      break;
    case FilterKind::kBigintValuesUsingBitmask:
      strKind = "BigintValuesUsingBitmask";
      break;
    case FilterKind::kDoubleRange:
      strKind = "DoubleRange";
      break;
    case FilterKind::kFloatRange:
      strKind = "FloatRange";
      break;
    case FilterKind::kBytesRange:
      strKind = "BytesRange";
      break;
    case FilterKind::kBytesValues:
      strKind = "BytesValues";
      break;
    case FilterKind::kBigintMultiRange:
      strKind = "BigintMultiRange";
      break;
    case FilterKind::kMultiRange:
      strKind = "MultiRange";
      break;
  };

  return fmt::format(
      "Filter({}, {}, {})",
      strKind,
      deterministic_ ? "deterministic" : "nondeterministic",
      nullAllowed_ ? "null allowed" : "null not allowed");
}

BigintValuesUsingBitmask::BigintValuesUsingBitmask(
    int64_t min,
    int64_t max,
    const std::vector<int64_t>& values,
    bool nullAllowed)
    : Filter(true, nullAllowed, FilterKind::kBigintValuesUsingBitmask),
      min_(min),
      max_(max) {
  VELOX_CHECK(min < max, "min must be less than max");
  VELOX_CHECK(values.size() > 1, "values must contain at least 2 entries");

  bitmask_.resize(max - min + 1);

  for (int64_t value : values) {
    bitmask_[value - min] = true;
  }
}

bool BigintValuesUsingBitmask::testInt64(int64_t value) const {
  if (value < min_ || value > max_) {
    return false;
  }
  return bitmask_[value - min_];
}

bool BigintValuesUsingBitmask::testInt64Range(
    int64_t min,
    int64_t max,
    bool hasNull) const {
  if (hasNull && nullAllowed_) {
    return true;
  }

  if (min == max) {
    return testInt64(min);
  }

  return !(min > max_ || max < min_);
}

BigintValuesUsingHashTable::BigintValuesUsingHashTable(
    int64_t min,
    int64_t max,
    const std::vector<int64_t>& values,
    bool nullAllowed)
    : Filter(true, nullAllowed, FilterKind::kBigintValuesUsingHashTable),
      min_(min),
      max_(max) {
  VELOX_CHECK(min < max, "min must be less than max");
  VELOX_CHECK(values.size() > 1, "values must contain at least 2 entries");

  auto size = 1u << (uint32_t)std::log2(values.size() * 3);
  hashTable_.resize(size);
  for (auto i = 0; i < size; ++i) {
    hashTable_[i] = kEmptyMarker;
  }
  for (auto value : values) {
    if (value == kEmptyMarker) {
      containsEmptyMarker_ = true;
    } else {
      auto position = ((value * M) & (size - 1));
      for (auto i = position; i < position + size; i++) {
        uint32_t index = i & (size - 1);
        if (hashTable_[index] == kEmptyMarker) {
          hashTable_[index] = value;
          break;
        }
      }
    }
  }
}

bool BigintValuesUsingHashTable::testInt64(int64_t value) const {
  if (containsEmptyMarker_ && value == kEmptyMarker) {
    return true;
  }
  if (value < min_ || value > max_) {
    return false;
  }
  uint32_t size = hashTable_.size();
  uint32_t pos = (value * M) & (size - 1);
  for (auto i = pos; i < pos + size; i++) {
    int32_t idx = i & (size - 1);
    int64_t l = hashTable_[idx];
    if (l == kEmptyMarker) {
      return false;
    }
    if (l == value) {
      return true;
    }
  }
  return false;
}

bool BigintValuesUsingHashTable::testInt64Range(
    int64_t min,
    int64_t max,
    bool hasNull) const {
  if (hasNull && nullAllowed_) {
    return true;
  }

  if (min == max) {
    return testInt64(min);
  }

  return !(min > max_ || max < min_);
}

namespace {
std::unique_ptr<Filter> nullOrFalse(bool nullAllowed) {
  if (nullAllowed) {
    return std::make_unique<IsNull>();
  }
  return std::make_unique<AlwaysFalse>();
}
} // namespace

std::unique_ptr<Filter> createBigintValues(
    const std::vector<int64_t>& values,
    bool nullAllowed) {
  if (values.empty()) {
    return nullOrFalse(nullAllowed);
  }

  if (values.size() == 1) {
    return std::make_unique<BigintRange>(
        values.front(), values.front(), nullAllowed);
  }

  int64_t min = values[0];
  int64_t max = values[0];
  for (int i = 1; i < values.size(); ++i) {
    if (values[i] > max) {
      max = values[i];
    } else if (values[i] < min) {
      min = values[i];
    }
  }
  // If bitmap would have more than 4 words per set bit, we prefer a
  // hash table. If bitmap fits in under 32 words, we use bitmap anyhow.
  int64_t range;
  bool overflow = __builtin_sub_overflow(max, min, &range);
  if (LIKELY(!overflow)) {
    if (range + 1 == values.size()) {
      return std::make_unique<BigintRange>(min, max, nullAllowed);
    }

    if (range < 32 * 64 || range < values.size() * 4 * 64) {
      return std::make_unique<BigintValuesUsingBitmask>(
          min, max, values, nullAllowed);
    }
  }
  return std::make_unique<BigintValuesUsingHashTable>(
      min, max, values, nullAllowed);
}

BigintMultiRange::BigintMultiRange(
    std::vector<std::unique_ptr<BigintRange>> ranges,
    bool nullAllowed)
    : Filter(true, nullAllowed, FilterKind::kBigintMultiRange),
      ranges_(std::move(ranges)) {
  VELOX_CHECK(!ranges_.empty(), "ranges is empty");
  VELOX_CHECK(ranges_.size() > 1, "should contain at least 2 ranges");
  for (const auto& range : ranges_) {
    lowerBounds_.push_back(range->lower());
  }
  for (int i = 1; i < lowerBounds_.size(); i++) {
    VELOX_CHECK(
        lowerBounds_[i] >= ranges_[i - 1]->upper(),
        "bigint ranges must not overlap");
  }
}

namespace {
int compareRanges(const char* lhs, size_t length, const std::string& rhs) {
  int size = std::min(length, rhs.length());
  int compare = memcmp(lhs, rhs.data(), size);
  if (compare) {
    return compare;
  }
  return length - rhs.size();
}
} // namespace

bool BytesRange::testBytes(const char* value, int32_t length) const {
  if (singleValue_) {
    if (length != lower_.size()) {
      return false;
    }
    return memcmp(value, lower_.data(), length) == 0;
  }
  if (!lowerUnbounded_) {
    int compare = compareRanges(value, length, lower_);
    if (compare < 0 || (lowerExclusive_ && compare == 0)) {
      return false;
    }
  }
  if (!upperUnbounded_) {
    int compare = compareRanges(value, length, upper_);
    return compare < 0 || (!upperExclusive_ && compare == 0);
  }
  return true;
}

bool BytesRange::testBytesRange(
    std::optional<std::string_view> min,
    std::optional<std::string_view> max,
    bool hasNull) const {
  if (hasNull && nullAllowed_) {
    return true;
  }

  if (min.has_value() && max.has_value() && min.value() == max.value()) {
    return testBytes(min->data(), min->length());
  }

  if (lowerUnbounded_) {
    // min > upper_
    return min.has_value() &&
        compareRanges(min->data(), min->length(), upper_) < 0;
  }

  if (upperUnbounded_) {
    // max < lower_
    return max.has_value() &&
        compareRanges(max->data(), max->length(), lower_) > 0;
  }

  // min > upper_
  if (min.has_value() &&
      compareRanges(min->data(), min->length(), upper_) > 0) {
    return false;
  }

  // max < lower_
  if (max.has_value() &&
      compareRanges(max->data(), max->length(), lower_) < 0) {
    return false;
  }
  return true;
}

bool BytesValues::testBytesRange(
    std::optional<std::string_view> min,
    std::optional<std::string_view> max,
    bool hasNull) const {
  if (hasNull && nullAllowed_) {
    return true;
  }

  if (min.has_value() && max.has_value() && min.value() == max.value()) {
    return testBytes(min->data(), min->length());
  }

  // min > upper_
  if (min.has_value() &&
      compareRanges(min->data(), min->length(), upper_) > 0) {
    return false;
  }

  // max < lower_
  if (max.has_value() &&
      compareRanges(max->data(), max->length(), lower_) < 0) {
    return false;
  }

  return true;
}

namespace {
int32_t binarySearch(const std::vector<int64_t>& values, int64_t value) {
  auto it = std::lower_bound(values.begin(), values.end(), value);
  if (it == values.end() || *it != value) {
    return -std::distance(values.begin(), it) - 1;
  } else {
    return std::distance(values.begin(), it);
  }
}
} // namespace

std::unique_ptr<Filter> BigintMultiRange::clone(
    std::optional<bool> nullAllowed) const {
  std::vector<std::unique_ptr<BigintRange>> ranges;
  ranges.reserve(ranges_.size());
  for (auto& range : ranges_) {
    ranges.emplace_back(std::make_unique<BigintRange>(*range));
  }
  if (nullAllowed) {
    return std::make_unique<BigintMultiRange>(
        std::move(ranges), nullAllowed.value());
  } else {
    return std::make_unique<BigintMultiRange>(std::move(ranges), nullAllowed_);
  }
}

bool BigintMultiRange::testInt64(int64_t value) const {
  int32_t i = binarySearch(lowerBounds_, value);
  if (i >= 0) {
    return true;
  }
  int place = (-i) - 1;
  if (place == 0) {
    // Below first
    return false;
  }
  // When value did not hit a lower bound of a filter, test with the filter
  // before the place where value would be inserted.
  return ranges_[place - 1]->testInt64(value);
}

bool BigintMultiRange::testInt64Range(int64_t min, int64_t max, bool hasNull)
    const {
  if (hasNull && nullAllowed_) {
    return true;
  }

  for (const auto& range : ranges_) {
    if (range->testInt64Range(min, max, hasNull)) {
      return true;
    }
  }

  return false;
}

std::unique_ptr<Filter> MultiRange::clone(
    std::optional<bool> nullAllowed) const {
  std::vector<std::unique_ptr<Filter>> filters;
  for (auto& filter : filters_) {
    filters.push_back(filter->clone());
  }

  if (nullAllowed) {
    return std::make_unique<MultiRange>(
        std::move(filters), nullAllowed.value(), nanAllowed_);
  } else {
    return std::make_unique<MultiRange>(
        std::move(filters), nullAllowed_, nanAllowed_);
  }
}

bool MultiRange::testDouble(double value) const {
  if (std::isnan(value)) {
    return nanAllowed_;
  }
  for (const auto& filter : filters_) {
    if (filter->testDouble(value)) {
      return true;
    }
  }
  return false;
}

bool MultiRange::testFloat(float value) const {
  if (std::isnan(value)) {
    return nanAllowed_;
  }
  for (const auto& filter : filters_) {
    if (filter->testFloat(value)) {
      return true;
    }
  }
  return false;
}

bool MultiRange::testBytes(const char* value, int32_t length) const {
  for (const auto& filter : filters_) {
    if (filter->testBytes(value, length)) {
      return true;
    }
  }
  return false;
}

bool MultiRange::testLength(int32_t length) const {
  for (const auto& filter : filters_) {
    if (filter->testLength(length)) {
      return true;
    }
  }
  return false;
}

bool MultiRange::testBytesRange(
    std::optional<std::string_view> min,
    std::optional<std::string_view> max,
    bool hasNull) const {
  if (hasNull && nullAllowed_) {
    return true;
  }

  for (const auto& filter : filters_) {
    if (filter->testBytesRange(min, max, hasNull)) {
      return true;
    }
  }

  return false;
}

std::unique_ptr<Filter> MultiRange::mergeWith(const Filter* other) const {
  switch (other->kind()) {
    // Rules of MultiRange with IsNull/IsNotNull
    // 1. MultiRange(nullAllowed=true) AND IS NULL => IS NULL
    // 2. MultiRange(nullAllowed=true) AND IS NOT NULL =>
    // MultiRange(nullAllowed=false)
    // 3. MultiRange(nullAllowed=false) AND IS NULL
    // => ALWAYS FALSE
    // 4. MultiRange(nullAllowed=false) AND IS NOT NULL
    // =>MultiRange(nullAllowed=false)
    case FilterKind::kAlwaysTrue:
    case FilterKind::kAlwaysFalse:
    case FilterKind::kIsNull:
      return other->mergeWith(this);
    case FilterKind::kIsNotNull:
      return this->clone(/*nullAllowed=*/false);
    case FilterKind::kDoubleRange:
    case FilterKind::kFloatRange:
    case FilterKind::kBytesRange:
    case FilterKind::kBytesValues:
      // TODO Implement
      VELOX_UNREACHABLE();
    case FilterKind::kMultiRange: {
      const MultiRange* multiRangeOther = static_cast<const MultiRange*>(other);
      bool bothNullAllowed = nullAllowed_ && other->testNull();
      bool bothNanAllowed = nanAllowed_ && multiRangeOther->nanAllowed_;
      std::vector<std::unique_ptr<Filter>> merged;
      for (auto const& filter : this->filters()) {
        for (auto const& filterOther : multiRangeOther->filters()) {
          auto innerMerged = filter->mergeWith(filterOther.get());
          switch (innerMerged->kind()) {
            case FilterKind::kAlwaysFalse:
            case FilterKind::kIsNull:
              break;
            default:
              merged.push_back(std::move(innerMerged));
          }
        }
      }

      if (merged.empty()) {
        return nullOrFalse(bothNullAllowed);
      } else if (merged.size() == 1) {
        return merged[0]->clone(bothNullAllowed);
      } else {
        return std::unique_ptr<Filter>(
            new MultiRange(std::move(merged), bothNullAllowed, bothNanAllowed));
      }
    }
    default:
      VELOX_UNREACHABLE();
  }
}

std::unique_ptr<Filter> IsNull::mergeWith(const Filter* other) const {
  VELOX_CHECK(other->isDeterministic());

  if (other->testNull()) {
    return this->clone();
  }

  return std::make_unique<AlwaysFalse>();
}

std::unique_ptr<Filter> IsNotNull::mergeWith(const Filter* other) const {
  switch (other->kind()) {
    case FilterKind::kAlwaysTrue:
    case FilterKind::kIsNotNull:
      return this->clone();
    case FilterKind::kAlwaysFalse:
    case FilterKind::kIsNull:
      return std::make_unique<AlwaysFalse>();
    default:
      return other->mergeWith(this);
  }
}

std::unique_ptr<Filter> BoolValue::mergeWith(const Filter* other) const {
  switch (other->kind()) {
    case FilterKind::kAlwaysTrue:
    case FilterKind::kAlwaysFalse:
    case FilterKind::kIsNull:
      return other->mergeWith(this);
    case FilterKind::kIsNotNull:
      return std::make_unique<BoolValue>(value_, false);
    case FilterKind::kBoolValue: {
      bool bothNullAllowed = nullAllowed_ && other->testNull();
      if (other->testBool(value_)) {
        return std::make_unique<BoolValue>(value_, bothNullAllowed);
      }

      return nullOrFalse(bothNullAllowed);
    }
    default:
      VELOX_UNREACHABLE();
  }
}

namespace {
std::unique_ptr<Filter> combineBigintRanges(
    std::vector<std::unique_ptr<BigintRange>> ranges,
    bool nullAllowed) {
  if (ranges.empty()) {
    return nullOrFalse(nullAllowed);
  }

  if (ranges.size() == 1) {
    return std::make_unique<BigintRange>(
        ranges.front()->lower(), ranges.front()->upper(), nullAllowed);
  }

  return std::make_unique<BigintMultiRange>(std::move(ranges), nullAllowed);
}

std::unique_ptr<BigintRange> toBigintRange(std::unique_ptr<Filter> filter) {
  return std::unique_ptr<BigintRange>(
      dynamic_cast<BigintRange*>(filter.release()));
}
} // namespace

std::unique_ptr<Filter> BigintRange::mergeWith(const Filter* other) const {
  switch (other->kind()) {
    case FilterKind::kAlwaysTrue:
    case FilterKind::kAlwaysFalse:
    case FilterKind::kIsNull:
      return other->mergeWith(this);
    case FilterKind::kIsNotNull:
      return std::make_unique<BigintRange>(lower_, upper_, false);
    case FilterKind::kBigintRange: {
      bool bothNullAllowed = nullAllowed_ && other->testNull();

      auto otherRange = static_cast<const BigintRange*>(other);

      auto lower = std::max(lower_, otherRange->lower_);
      auto upper = std::min(upper_, otherRange->upper_);

      if (lower <= upper) {
        return std::make_unique<BigintRange>(lower, upper, bothNullAllowed);
      }

      return nullOrFalse(bothNullAllowed);
    }
    case FilterKind::kBigintValuesUsingBitmask:
    case FilterKind::kBigintValuesUsingHashTable:
      return other->mergeWith(this);
    case FilterKind::kBigintMultiRange: {
      auto otherMultiRange = dynamic_cast<const BigintMultiRange*>(other);
      std::vector<std::unique_ptr<BigintRange>> newRanges;
      for (const auto& range : otherMultiRange->ranges()) {
        auto merged = this->mergeWith(range.get());
        if (merged->kind() == FilterKind::kBigintRange) {
          newRanges.push_back(toBigintRange(std::move(merged)));
        } else {
          VELOX_CHECK(merged->kind() == FilterKind::kAlwaysFalse);
        }
      }

      bool bothNullAllowed = nullAllowed_ && other->testNull();
      return combineBigintRanges(std::move(newRanges), bothNullAllowed);
    }
    default:
      VELOX_UNREACHABLE();
  }
}

std::unique_ptr<Filter> BigintValuesUsingHashTable::mergeWith(
    const Filter* other) const {
  switch (other->kind()) {
    case FilterKind::kAlwaysTrue:
    case FilterKind::kAlwaysFalse:
    case FilterKind::kIsNull:
      return other->mergeWith(this);
    case FilterKind::kIsNotNull:
      return std::make_unique<BigintValuesUsingHashTable>(*this, false);
    case FilterKind::kBigintRange: {
      auto otherRange = dynamic_cast<const BigintRange*>(other);
      auto min = std::max(min_, otherRange->lower());
      auto max = std::min(max_, otherRange->upper());

      return mergeWith(min, max, other);
    }
    case FilterKind::kBigintValuesUsingHashTable: {
      auto otherValues = dynamic_cast<const BigintValuesUsingHashTable*>(other);
      auto min = std::max(min_, otherValues->min());
      auto max = std::min(max_, otherValues->max());

      return mergeWith(min, max, other);
    }
    case FilterKind::kBigintValuesUsingBitmask:
      return other->mergeWith(this);
    case FilterKind::kBigintMultiRange: {
      auto otherMultiRange = dynamic_cast<const BigintMultiRange*>(other);

      std::vector<int64_t> valuesToKeep;
      if (containsEmptyMarker_ && other->testInt64(kEmptyMarker)) {
        valuesToKeep.emplace_back(kEmptyMarker);
      }
      for (const auto& range : otherMultiRange->ranges()) {
        auto min = std::max(min_, range->lower());
        auto max = std::min(max_, range->upper());

        if (min <= max) {
          for (int64_t v : hashTable_) {
            if (v != kEmptyMarker && range->testInt64(v)) {
              valuesToKeep.emplace_back(v);
            }
          }
        }
      }

      bool bothNullAllowed = nullAllowed_ && other->testNull();
      return createBigintValues(valuesToKeep, bothNullAllowed);
    }
    default:
      VELOX_UNREACHABLE();
  }
}

std::unique_ptr<Filter> BigintValuesUsingHashTable::mergeWith(
    int64_t min,
    int64_t max,
    const Filter* other) const {
  bool bothNullAllowed = nullAllowed_ && other->testNull();

  if (max < min) {
    return nullOrFalse(bothNullAllowed);
  }

  if (max == min) {
    if (testInt64(min) && other->testInt64(min)) {
      return std::make_unique<BigintRange>(min, min, bothNullAllowed);
    }

    return nullOrFalse(bothNullAllowed);
  }

  std::vector<int64_t> valuesToKeep;
  valuesToKeep.reserve(max - min + 1);
  if (containsEmptyMarker_ && other->testInt64(kEmptyMarker)) {
    valuesToKeep.emplace_back(kEmptyMarker);
  }

  for (int64_t v : hashTable_) {
    if (v != kEmptyMarker && other->testInt64(v)) {
      valuesToKeep.emplace_back(v);
    }
  }

  return createBigintValues(valuesToKeep, bothNullAllowed);
}

std::unique_ptr<Filter> BigintValuesUsingBitmask::mergeWith(
    const Filter* other) const {
  switch (other->kind()) {
    case FilterKind::kAlwaysTrue:
    case FilterKind::kAlwaysFalse:
    case FilterKind::kIsNull:
      return other->mergeWith(this);
    case FilterKind::kIsNotNull:
      return std::make_unique<BigintValuesUsingBitmask>(*this, false);
    case FilterKind::kBigintRange: {
      auto otherRange = dynamic_cast<const BigintRange*>(other);

      auto min = std::max(min_, otherRange->lower());
      auto max = std::min(max_, otherRange->upper());

      return mergeWith(min, max, other);
    }
    case FilterKind::kBigintValuesUsingHashTable: {
      auto otherValues = dynamic_cast<const BigintValuesUsingHashTable*>(other);

      auto min = std::max(min_, otherValues->min());
      auto max = std::min(max_, otherValues->max());

      return mergeWith(min, max, other);
    }
    case FilterKind::kBigintValuesUsingBitmask: {
      auto otherValues = dynamic_cast<const BigintValuesUsingBitmask*>(other);

      auto min = std::max(min_, otherValues->min_);
      auto max = std::min(max_, otherValues->max_);

      return mergeWith(min, max, other);
    }
    case FilterKind::kBigintMultiRange: {
      auto otherMultiRange = dynamic_cast<const BigintMultiRange*>(other);

      std::vector<int64_t> valuesToKeep;
      for (const auto& range : otherMultiRange->ranges()) {
        auto min = std::max(min_, range->lower());
        auto max = std::min(max_, range->upper());
        for (auto i = min; i <= max; ++i) {
          if (bitmask_[i - min_] && range->testInt64(i)) {
            valuesToKeep.push_back(i);
          }
        }
      }

      bool bothNullAllowed = nullAllowed_ && other->testNull();
      return createBigintValues(valuesToKeep, bothNullAllowed);
    }
    default:
      VELOX_UNREACHABLE();
  }
}

std::unique_ptr<Filter> BigintValuesUsingBitmask::mergeWith(
    int64_t min,
    int64_t max,
    const Filter* other) const {
  bool bothNullAllowed = nullAllowed_ && other->testNull();

  std::vector<int64_t> valuesToKeep;
  for (auto i = min; i <= max; ++i) {
    if (bitmask_[i - min_] && other->testInt64(i)) {
      valuesToKeep.push_back(i);
    }
  }
  return createBigintValues(valuesToKeep, bothNullAllowed);
}

std::unique_ptr<Filter> BigintMultiRange::mergeWith(const Filter* other) const {
  switch (other->kind()) {
    case FilterKind::kAlwaysTrue:
    case FilterKind::kAlwaysFalse:
    case FilterKind::kIsNull:
      return other->mergeWith(this);
    case FilterKind::kIsNotNull: {
      std::vector<std::unique_ptr<BigintRange>> ranges;
      ranges.reserve(ranges_.size());
      for (auto& range : ranges_) {
        ranges.push_back(std::make_unique<BigintRange>(*range));
      }
      return std::make_unique<BigintMultiRange>(std::move(ranges), false);
    }
    case FilterKind::kBigintRange:
    case FilterKind::kBigintValuesUsingBitmask:
    case FilterKind::kBigintValuesUsingHashTable: {
      return other->mergeWith(this);
    }
    case FilterKind::kBigintMultiRange: {
      std::vector<std::unique_ptr<BigintRange>> newRanges;
      for (const auto& range : ranges_) {
        auto merged = range->mergeWith(other);
        if (merged->kind() == FilterKind::kBigintRange) {
          newRanges.push_back(toBigintRange(std::move(merged)));
        } else if (merged->kind() == FilterKind::kBigintMultiRange) {
          auto mergedMultiRange = dynamic_cast<BigintMultiRange*>(merged.get());
          for (const auto& newRange : mergedMultiRange->ranges_) {
            newRanges.push_back(toBigintRange(newRange->clone()));
          }
        } else {
          VELOX_CHECK(merged->kind() == FilterKind::kAlwaysFalse);
        }
      }

      bool bothNullAllowed = nullAllowed_ && other->testNull();
      if (newRanges.empty()) {
        return nullOrFalse(bothNullAllowed);
      }

      if (newRanges.size() == 1) {
        return std::make_unique<BigintRange>(
            newRanges.front()->lower(),
            newRanges.front()->upper(),
            bothNullAllowed);
      }

      return std::make_unique<BigintMultiRange>(
          std::move(newRanges), bothNullAllowed);
    }
    default:
      VELOX_UNREACHABLE();
  }
}
} // namespace facebook::velox::common
