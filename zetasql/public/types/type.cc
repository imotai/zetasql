//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "zetasql/public/types/type.h"

#include <stddef.h>
#include <stdlib.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "zetasql/base/logging.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/descriptor.h"
#include "zetasql/common/errors.h"
#include "zetasql/public/language_options.h"
#include "zetasql/public/options.pb.h"
#include "zetasql/public/type.pb.h"
#include "zetasql/public/types/array_type.h"
#include "zetasql/public/types/map_type.h"
#include "zetasql/public/types/measure_type.h"
#include "zetasql/public/types/range_type.h"
#include "zetasql/public/types/simple_type.h"
#include "zetasql/public/types/struct_type.h"
#include "zetasql/public/types/type_factory.h"
#include "zetasql/public/types/type_parameters.h"
#include "zetasql/public/value.pb.h"
#include "zetasql/public/value_content.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "absl/hash/hash.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/variant.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {

namespace {
struct TypeKindInfo {
  // Nonexisting type kinds have NULL name pointer.
  const char* name = nullptr;

  // This is used to determine the cost of coercing from one type to another,
  // computed as the difference in the two type values.  Note that this is
  // different from <specificity> in that FLOAT and DOUBLE are reversed
  // in cost ordering, reflecting that we prefer to coerce INTs to DOUBLE rather
  // than FLOAT.  Additionally, we prefer coercing from STRING/INT to TIMESTAMP
  // rather than any of the legacy timestamp types (TIMESTAMP_MICROS, etc.).
  // Date/time related types can only be coerced from STRING, we prefer coercing
  // from STRING to the closest of these date/time types before coercing to
  // date/time types that are earlier in the list. When
  // FEATURE_V_1_4_IMPLICIT_COERCION_STRING_LITERAL_TO_BYTES is enabled, STRING
  // literals can also coerce to BYTES, at a lower precedence than to other
  // related types.
  int cost = 0;

  // Type specificity is used for Supertype analysis.  To evaluate the
  // Supertype of multiple types, we get the sets of supertypes associated
  // with each type, and the common supertype is the most specific type
  // in their intersection.
  int specificity = 0;

  bool simple = false;
};

using TypeKindInfoList = std::array<TypeKindInfo, TypeKind_ARRAYSIZE>;
constexpr TypeKindInfoList MakeTypeKindInfoList() {
  TypeKindInfoList kinds = {};

  // None of the built-in type names/aliases should start with "[a-zA-Z]_",
  // which is reserved for user-defined objects.
  kinds[TYPE_UNKNOWN] = {
      .name = "UNKNOWN",
      .cost = 0,
      .specificity = 0,
      .simple = false,
  };
  kinds[TYPE_INT32] = {
      .name = "INT32",
      .cost = 12,
      .specificity = 12,
      .simple = true,
  };
  kinds[TYPE_INT64] = {
      .name = "INT64",
      .cost = 14,
      .specificity = 14,
      .simple = true,
  };
  kinds[TYPE_UINT32] = {
      .name = "UINT32",
      .cost = 11,
      .specificity = 11,
      .simple = true,
  };
  kinds[TYPE_UINT64] = {
      .name = "UINT64",
      .cost = 13,
      .specificity = 13,
      .simple = true,
  };
  kinds[TYPE_BOOL] = {
      .name = "BOOL",
      .cost = 10,
      .specificity = 10,
      .simple = true,
  };
  kinds[TYPE_FLOAT] = {
      .name = "FLOAT",
      .cost = 18,
      .specificity = 17,
      .simple = true,
  };
  kinds[TYPE_DOUBLE] = {
      .name = "DOUBLE",
      .cost = 17,
      .specificity = 18,
      .simple = true,
  };
  kinds[TYPE_STRING] = {
      .name = "STRING",
      .cost = 19,
      .specificity = 19,
      .simple = true,
  };
  kinds[TYPE_BYTES] = {
      .name = "BYTES",
      .cost = 38,
      .specificity = 20,
      .simple = true,
  };
  kinds[TYPE_DATE] = {
      .name = "DATE",
      .cost = 9,
      .specificity = 7,
      .simple = true,
  };

  kinds[TYPE_ENUM] = {
      .name = "ENUM",
      .cost = 1,
      .specificity = 1,
      .simple = false,
  };
  kinds[TYPE_ARRAY] = {
      .name = "ARRAY",
      .cost = 23,
      .specificity = 23,
      .simple = false,
  };
  kinds[TYPE_STRUCT] = {
      .name = "STRUCT",
      .cost = 22,
      .specificity = 22,
      .simple = false,
  };
  kinds[TYPE_PROTO] = {
      .name = "PROTO",
      .cost = 21,
      .specificity = 21,
      .simple = false,
  };
  kinds[TYPE_TIMESTAMP] = {
      .name = "TIMESTAMP",
      .cost = 8,
      .specificity = 2,
      .simple = true,
  };
  kinds[TYPE_TIMESTAMP_PICOS] = {
      .name = "TIMESTAMP_PICOS",
      .cost = 35,
      .specificity = 35,
      .simple = true,
  };
  kinds[TYPE_TIME] = {
      .name = "TIME",
      .cost = 2,
      .specificity = 8,
      .simple = true,
  };
  kinds[TYPE_DATETIME] = {
      .name = "DATETIME",
      .cost = 3,
      .specificity = 9,
      .simple = true,
  };
  kinds[TYPE_GEOGRAPHY] = {
      .name = "GEOGRAPHY",
      .cost = 24,
      .specificity = 24,
      .simple = true,
  };
  kinds[TYPE_NUMERIC] = {
      .name = "NUMERIC",
      .cost = 15,
      .specificity = 15,
      .simple = true,
  };
  kinds[TYPE_BIGNUMERIC] = {
      .name = "BIGNUMERIC",
      .cost = 16,
      .specificity = 16,
      .simple = true,
  };
  kinds[TYPE_EXTENDED] = {
      .name = "EXTENDED",
      .cost = 25,
      .specificity = 25,
      .simple = false,
  };
  kinds[TYPE_JSON] = {
      .name = "JSON",
      .cost = 26,
      .specificity = 26,
      .simple = true,
  };
  kinds[TYPE_INTERVAL] = {
      .name = "INTERVAL",
      .cost = 27,
      .specificity = 27,
      .simple = true,
  };
  kinds[TYPE_TOKENLIST] = {
      .name = "TOKENLIST",
      .cost = 28,
      .specificity = 28,
      .simple = true,
  };
  kinds[TYPE_RANGE] = {
      .name = "RANGE",
      .cost = 29,
      .specificity = 29,
      .simple = false,
  };
  kinds[TYPE_GRAPH_ELEMENT] = {
      .name = "GRAPH_ELEMENT",
      .cost = 30,
      .specificity = 30,
      .simple = false,
  };
  kinds[TYPE_GRAPH_PATH] = {
      .name = "GRAPH_PATH",
      .cost = 33,
      .specificity = 33,
      .simple = false,
  };
  kinds[TYPE_MAP] = {
      .name = "MAP",
      .cost = 31,
      .specificity = 31,
      .simple = false,
  };
  kinds[TYPE_UUID] = {
      .name = "UUID",
      .cost = 32,
      .specificity = 32,
      .simple = true,
  };
  kinds[TYPE_MEASURE] = {
      .name = "MEASURE",
      .cost = 34,
      .specificity = 34,
      .simple = false,
  };

  return kinds;
}

// Global static to avoid overhead of thread-safe local statics initialization.
constexpr TypeKindInfoList kTypeKindInfoList = MakeTypeKindInfoList();

// Returns NULL if TypeKindInfo is not defined for the given `kind`.
const TypeKindInfo* FindTypeKindInfo(TypeKind kind) {
  if (ABSL_PREDICT_FALSE(kind >= kTypeKindInfoList.size())) {
    return nullptr;
  }
  if (ABSL_PREDICT_FALSE(kTypeKindInfoList[kind].name == nullptr)) {
    return nullptr;
  }
  return &kTypeKindInfoList[kind];
}

}  // namespace

Type::Type(const TypeFactory* factory, TypeKind kind)
    : type_store_(internal::TypeStoreHelper::GetTypeStore(factory)),
      kind_(kind) {}

// static
bool Type::IsSimpleType(TypeKind kind) {
  const TypeKindInfo* info = FindTypeKindInfo(kind);
  if (ABSL_PREDICT_TRUE(info != nullptr)) {
    return info->simple;
  }
  return false;
}

bool Type::IsSupportedSimpleTypeKind(TypeKind kind,
                                     const LanguageOptions& language_options) {
  ABSL_DCHECK(IsSimpleType(kind));
  const zetasql::Type* type = types::TypeFromSimpleTypeKind(kind);
  return type->IsSupportedType(language_options);
}

TypeKind Type::ResolveBuiltinTypeNameToKindIfSimple(absl::string_view type_name,
                                                    ProductMode mode) {
  return SimpleType::GetTypeKindIfSimple(type_name, mode);
}

TypeKind Type::ResolveBuiltinTypeNameToKindIfSimple(
    absl::string_view type_name, const LanguageOptions& language_options) {
  return SimpleType::GetTypeKindIfSimple(
      type_name, language_options.product_mode(),
      &language_options.GetEnabledLanguageFeatures());
}

std::string Type::TypeKindToString(TypeKind kind, ProductMode mode,
                                   bool use_external_float32) {
  // Note that for types not externally supported we still want to produce
  // the internal names for them.  This is because during development
  // we want error messages to indicate what the unsupported type actually
  // is as an aid in debugging.  When used in production in external mode,
  // those internal names should never actually be reachable.
  const TypeKindInfo* info = FindTypeKindInfo(kind);
  if (ABSL_PREDICT_TRUE(info != nullptr)) {
    if (mode == PRODUCT_EXTERNAL && kind == TYPE_DOUBLE) {
      return "FLOAT64";
    } else if (mode == PRODUCT_EXTERNAL && use_external_float32 &&
               kind == TYPE_FLOAT) {
      return "FLOAT32";
    }
    return info->name;
  }
  return absl::StrCat("INVALID_TYPE_KIND(", kind, ")");
}

std::string Type::TypeKindListToString(const std::vector<TypeKind>& kinds,
                                       ProductMode mode,
                                       bool use_external_float32) {
  std::vector<std::string> kind_strings;
  kind_strings.reserve(kinds.size());
  for (const TypeKind& kind : kinds) {
    kind_strings.push_back(TypeKindToString(kind, mode, use_external_float32));
  }
  return absl::StrJoin(kind_strings, ", ");
}

std::string Type::TypeListToString(TypeListView types, ProductMode mode,
                                   bool use_external_float32) {
  std::vector<std::string> type_strings;
  type_strings.reserve(types.size());
  for (const Type* type : types) {
    type_strings.push_back(type->ShortTypeName(mode, use_external_float32));
  }
  return absl::StrJoin(type_strings, ", ");
}

Type::FormatValueContentOptions
Type::FormatValueContentOptions::IncreaseIndent() {
  FormatValueContentOptions ret = *this;
  ret.indent += kIndentStep;
  // `force_type` only applies at the topmost level
  ret.force_type_at_top_level = false;
  return ret;
}

int Type::KindSpecificity(TypeKind kind) {
  const TypeKindInfo* info = FindTypeKindInfo(kind);
  if (ABSL_PREDICT_TRUE(info != nullptr)) {
    return info->specificity;
  }

  ABSL_LOG(FATAL) << "Out of range: " << kind;
}

static int KindCost(TypeKind kind) {
  const TypeKindInfo* info = FindTypeKindInfo(kind);
  if (ABSL_PREDICT_TRUE(info != nullptr)) {
    return info->cost;
  }

  ABSL_LOG(FATAL) << "Out of range: " << kind;
}

int Type::GetTypeCoercionCost(TypeKind kind1, TypeKind kind2) {
  return abs(KindCost(kind1) - KindCost(kind2));
}

bool Type::KindSpecificityLess(TypeKind kind1, TypeKind kind2) {
  ABSL_DCHECK_NE(kind1, TypeKind::TYPE_EXTENDED);
  ABSL_DCHECK_NE(kind2, TypeKind::TYPE_EXTENDED);

  return KindSpecificity(kind1) < KindSpecificity(kind2);
}

bool Type::TypeSpecificityLess(const Type* t1, const Type* t2) {
  return KindSpecificityLess(t1->kind(), t2->kind());
}

absl::Status Type::SerializeToProtoAndFileDescriptors(
    TypeProto* type_proto,
    google::protobuf::FileDescriptorSet* file_descriptor_set,
    std::set<const google::protobuf::FileDescriptor*>* file_descriptors) const {
  type_proto->Clear();

  FileDescriptorSetMap file_descriptor_set_map;
  if (file_descriptors != nullptr && !file_descriptors->empty()) {
    const google::protobuf::DescriptorPool* pool = (*file_descriptors->begin())->pool();
    std::unique_ptr<FileDescriptorEntry> file_descriptor_entry(
        new FileDescriptorEntry);
    file_descriptor_entry->descriptor_set_index = 0;
    if (file_descriptor_set != nullptr) {
      file_descriptor_entry->file_descriptor_set.Swap(file_descriptor_set);
    }
    file_descriptor_entry->file_descriptors.swap(*file_descriptors);
    file_descriptor_set_map.emplace(pool, std::move(file_descriptor_entry));
  }
  // Optimization: skip building the descriptor sets if they would not
  // affect the output at all.
  BuildFileDescriptorSetMapOptions options;
  options.build_file_descriptor_sets =
      file_descriptor_set != nullptr || file_descriptors != nullptr;

  // No limit on FileDescriptorSet size.
  ZETASQL_RETURN_IF_ERROR(SerializeToProtoAndDistinctFileDescriptorsImpl(
      options, type_proto, &file_descriptor_set_map));
  if (file_descriptor_set_map.size() > 1) {
    return MakeSqlError()
           << "Unable to serialize descriptors spanning multiple "
              "DescriptorPools into a single FileDescriptorSet. "
              "Use SerializeToProtoAndDistinctFileDescriptors "
              "instead.";
  } else if (!file_descriptor_set_map.empty()) {
    const std::unique_ptr<FileDescriptorEntry>& file_descriptor_entry =
        file_descriptor_set_map.begin()->second;
    if (file_descriptor_set != nullptr) {
      file_descriptor_set->Swap(&file_descriptor_entry->file_descriptor_set);
    }
    if (file_descriptors != nullptr) {
      file_descriptors->swap(file_descriptor_entry->file_descriptors);
    }
  }
  return absl::OkStatus();
}

absl::Status Type::SerializeToProtoAndDistinctFileDescriptors(
    TypeProto* type_proto,
    FileDescriptorSetMap* file_descriptor_set_map) const {
  // No limit on FileDescriptorSet size.
  return SerializeToProtoAndDistinctFileDescriptors(
      type_proto,
      /*file_descriptor_sets_max_size_bytes=*/std::optional<int64_t>(),
      file_descriptor_set_map);
}

absl::Status Type::SerializeToProtoAndDistinctFileDescriptors(
    TypeProto* type_proto,
    std::optional<int64_t> file_descriptor_sets_max_size_bytes,
    FileDescriptorSetMap* file_descriptor_set_map) const {
  ZETASQL_RET_CHECK(file_descriptor_set_map != nullptr);
  type_proto->Clear();
  BuildFileDescriptorSetMapOptions options;
  options.file_descriptor_sets_max_size_bytes =
      file_descriptor_sets_max_size_bytes;
  return SerializeToProtoAndDistinctFileDescriptorsImpl(
      options, type_proto, file_descriptor_set_map);
}

absl::Status Type::SerializeToSelfContainedProto(
    TypeProto* type_proto) const {
  type_proto->Clear();
  FileDescriptorSetMap file_descriptor_set_map;
  // No limit on FileDescriptorSet size.  TODO: Allow a limit to be
  // provided here as well.  Maybe this should just call the non-Impl version.
  ZETASQL_RETURN_IF_ERROR(SerializeToProtoAndDistinctFileDescriptorsImpl(
      {}, type_proto, &file_descriptor_set_map));
  // Determine the order of the FileDescriptorSets in the TypeProto.
  std::vector<google::protobuf::FileDescriptorSet*> file_descriptor_sets;
  file_descriptor_sets.resize(file_descriptor_set_map.size());
  for (const auto& pool_and_file_set : file_descriptor_set_map) {
    file_descriptor_sets[pool_and_file_set.second->descriptor_set_index] =
        &pool_and_file_set.second->file_descriptor_set;
  }
  // Now add all of them in order.
  for (google::protobuf::FileDescriptorSet* file_descriptor_set : file_descriptor_sets) {
    ZETASQL_RET_CHECK(file_descriptor_set != nullptr);
    type_proto->add_file_descriptor_set()->Swap(file_descriptor_set);
  }
  return absl::OkStatus();
}

std::string Type::ShortTypeName(ProductMode mode) const {
  return TypeName(mode);
}

std::string Type::DebugString(bool details) const {
  std::string debug_string;

  TypeOrStringVector stack;
  stack.push_back(this);
  while (!stack.empty()) {
    const auto stack_entry = stack.back();
    stack.pop_back();
    if (std::holds_alternative<std::string>(stack_entry)) {
      absl::StrAppend(&debug_string, std::get<std::string>(stack_entry));
      continue;
    }
    std::get<const Type*>(stack_entry)
        ->DebugStringImpl(details, &stack, &debug_string);
  }
  return debug_string;
}

std::string Type::CapitalizedName() const {
  switch (kind()) {
    case TYPE_INT32:
      return "Int32";
    case TYPE_INT64:
      return "Int64";
    case TYPE_UINT32:
      return "Uint32";
    case TYPE_UINT64:
      return "Uint64";
    case TYPE_BOOL:
      return "Bool";
    case TYPE_FLOAT:
      return "Float";
    case TYPE_DOUBLE:
      return "Double";
    case TYPE_STRING:
      return "String";
    case TYPE_BYTES:
      return "Bytes";
    case TYPE_DATE:
      return "Date";
    case TYPE_TIMESTAMP:
      return "Timestamp";
    case TYPE_TIMESTAMP_PICOS:
      return "Timestamp_picos";
    case TYPE_TIME:
      return "Time";
    case TYPE_DATETIME:
      return "Datetime";
    case TYPE_INTERVAL:
      return "Interval";
    case TYPE_GEOGRAPHY:
      return "Geography";
    case TYPE_NUMERIC:
      return "Numeric";
    case TYPE_BIGNUMERIC:
      return "BigNumeric";
    case TYPE_JSON:
      return "Json";
    case TYPE_TOKENLIST:
      return "TokenList";
    case TYPE_RANGE:
      // TODO: Consider moving to the types library and audit use of
      // DebugString.
      // TODO: Add tests for this logic after implementing range
      // in zetasql::Value.
      return absl::StrCat("Range<",
                          this->AsRange()->element_type()->DebugString(), ">");
    case TYPE_MAP:
      return absl::StrCat("Map<", GetMapKeyType(this)->CapitalizedName(), ", ",
                          GetMapValueType(this)->CapitalizedName(), ">");
    case TYPE_ENUM: {
      if (AsEnum()->IsOpaque()) {
        return AsEnum()->ShortTypeName(ProductMode::PRODUCT_EXTERNAL);
      } else {
        return absl::StrCat("Enum<", AsEnum()->enum_descriptor()->full_name(),
                            ">");
      }
    }
    case TYPE_ARRAY:
      // TODO: Consider moving to the types library and audit use of
      // DebugString.
      return absl::StrCat("Array<",
                          this->AsArray()->element_type()->DebugString(), ">");
    case TYPE_STRUCT:
      return "Struct";
    case TYPE_PROTO:
      ABSL_CHECK(AsProto()->descriptor() != nullptr);
      return absl::StrCat("Proto<", AsProto()->descriptor()->full_name(), ">");
    case TYPE_GRAPH_ELEMENT:
      return AsGraphElement()->IsNode() ? "GraphNode" : "GraphEdge";
    case TYPE_GRAPH_PATH:
      return "GraphPath";
    case TYPE_UUID:
      return "Uuid";
    case TYPE_MEASURE:
      return absl::StrCat("Measure<",
                          AsMeasure()->result_type()->CapitalizedName(), ">");
    case TYPE_EXTENDED:
      // TODO: move this logic into an appropriate function of
      // Type's interface.
      return ShortTypeName(ProductMode::PRODUCT_EXTERNAL);
    case TYPE_UNKNOWN:
    case __TypeKind__switch_must_have_a_default__:
      ABSL_LOG(FATAL) << "Unexpected type kind expected internally only: " << kind();
  }
}

bool Type::SupportsGrouping(const LanguageOptions& language_options,
                            std::string* type_description) const {
  const Type* no_grouping_type;
  const bool supports_grouping =
      this->SupportsGroupingImpl(language_options, &no_grouping_type);
  if (!supports_grouping && type_description != nullptr) {
    if (no_grouping_type == this) {
      *type_description =
          TypeKindToString(this->kind(), language_options.product_mode());
    } else {
      *type_description = absl::StrCat(
          TypeKindToString(this->kind(), language_options.product_mode()),
          " containing ",
          TypeKindToString(no_grouping_type->kind(),
                           language_options.product_mode()));
    }
  }
  return supports_grouping;
}

bool Type::SupportsGroupingImpl(const LanguageOptions& language_options,
                                const Type** no_grouping_type) const {
  if (no_grouping_type != nullptr) {
    *no_grouping_type = this;
  }
  return false;
}

bool Type::SupportsPartitioning(const LanguageOptions& language_options,
                                std::string* type_description) const {
  const Type* no_partitioning_type;
  const bool supports_partitioning =
      this->SupportsPartitioningImpl(language_options, &no_partitioning_type);

  if (!supports_partitioning && type_description != nullptr) {
    if (no_partitioning_type == this) {
      *type_description =
          TypeKindToString(this->kind(), language_options.product_mode());
    } else {
      *type_description = absl::StrCat(
          TypeKindToString(this->kind(), language_options.product_mode()),
          " containing ",
          TypeKindToString(no_partitioning_type->kind(),
                           language_options.product_mode()));
    }
  }
  return supports_partitioning;
}

bool Type::SupportsPartitioningImpl(const LanguageOptions& language_options,
                                    const Type** no_partitioning_type) const {
  bool supports_partitioning =
      !this->IsGeography() && !this->IsFloatingPoint() && !this->IsJson();
  if (this->IsTokenList()) supports_partitioning = false;

  if (no_partitioning_type != nullptr) {
    *no_partitioning_type = supports_partitioning ? nullptr : this;
  }
  return supports_partitioning;
}

bool Type::SupportsOrdering(const LanguageOptions& language_options,
                            std::string* type_description) const {
  bool supports_ordering = !IsGeography() && !IsJson();
  if (this->IsTokenList()) supports_ordering = false;
  if (supports_ordering) return true;
  if (type_description != nullptr) {
    *type_description = TypeKindToString(this->kind(),
                                         language_options.product_mode());
  }
  return false;
}

bool Type::SupportsOrdering() const {
  return SupportsOrdering(LanguageOptions(), /*type_description=*/nullptr);
}

// Array type equality support is controlled by the language option
// FEATURE_V_1_1_ARRAY_EQUALITY. To test if 'type' supports equality,
// checks the type recursively as array types can be nested under
// struct types or vice versa.
bool Type::SupportsEquality(
    const LanguageOptions& language_options) const {
  if (this->IsArray()) {
    if (language_options.LanguageFeatureEnabled(FEATURE_V_1_1_ARRAY_EQUALITY)) {
      return this->AsArray()->element_type()->SupportsEquality(
          language_options);
    } else {
      return false;
    }
  } else if (this->IsStruct()) {
    for (const StructField& field : this->AsStruct()->fields()) {
      if (!field.type->SupportsEquality(language_options)) {
        return false;
      }
    }
    return true;
  }
  return this->SupportsEquality();
}

bool Type::SupportsReturning(const LanguageOptions& language_options,
                             std::string* type_description) const {
  switch (this->kind()) {
    case TYPE_ARRAY:
      return this->AsArray()->element_type()->SupportsReturning(
          language_options, type_description);
    case TYPE_STRUCT:
      for (const StructField& field : this->AsStruct()->fields()) {
        if (!field.type->SupportsReturning(language_options,
                                           type_description)) {
          return false;
        }
      }
      return true;
    case TYPE_MAP:
      return GetMapKeyType(this)->SupportsReturning(language_options,
                                                    type_description) &&
             GetMapValueType(this)->SupportsReturning(language_options,
                                                      type_description);
    case TYPE_GRAPH_ELEMENT:
    case TYPE_GRAPH_PATH:
      if (type_description != nullptr) {
        *type_description =
            TypeKindToString(this->kind(), language_options.product_mode(),
                             !language_options.LanguageFeatureEnabled(
                                 FEATURE_V_1_4_DISABLE_FLOAT32));
      }
      return false;
    case TYPE_MEASURE:
      // TODO: b/350555383 - We probably want to allow returning Measure in some
      // places which are disallowed when SupportsReturning() is false, for
      // example in the output column list from a TVF or View.
      return false;
    default:
      return true;
  }
}

void Type::CopyValueContent(const ValueContent& from, ValueContent* to) const {
  *to = from;
}

absl::HashState Type::Hash(absl::HashState state) const {
  // Hash a type's kind.
  state = absl::HashState::combine(std::move(state), kind());

  // Hash a type's parameter.
  return HashTypeParameter(std::move(state));
}

absl::Status Type::TypeMismatchError(const ValueProto& value_proto) const {
  return absl::Status(
      absl::StatusCode::kInternal,
      absl::StrCat(
          "Type mismatch: provided type ", DebugString(), " but proto <",
          value_proto.ShortDebugString(),
          "> doesn't have field of that type and is not null"));
}

bool TypeEquals::operator()(const Type* const type1,
                            const Type* const type2) const {
  if (type1 == type2) {
    // Note that two nullptr Type pointers will compare to TRUE.
    return true;
  }
  if (type1 == nullptr || type2 == nullptr) {
    // If one is nullptr and the other not nullptr, then they cannot be equal.
    return false;
  }
  return type1->Equals(type2);
}

bool TypeEquivalent::operator()(const Type* const type1,
                                const Type* const type2) const {
  if (type1 == type2) {
    // Note that two nullptr Type pointers will compare to TRUE.
    return true;
  }
  if (type1 == nullptr || type2 == nullptr) {
    // If one is nullptr and the other not nullptr, then they cannot be equal.
    return false;
  }
  return type1->Equivalent(type2);
}

size_t TypeHash::operator()(const Type* const type) const {
  if (type == nullptr) {
    return 17 * 23;  // Some random number to represent nullptr.
  }

  return absl::Hash<Type>()(*type);
}

absl::StatusOr<TypeParameters> Type::ValidateAndResolveTypeParameters(
    const std::vector<TypeParameterValue>& type_parameter_values,
    ProductMode mode) const {
  return MakeSqlError() << "Type " << ShortTypeName(mode)
                        << " does not support type parameters";
}

absl::Status Type::ValidateResolvedTypeParameters(
    const TypeParameters& type_parameters, ProductMode mode) const {
  ZETASQL_RET_CHECK(type_parameters.IsEmpty())
      << "Type " << ShortTypeName(mode) << " does not support type parameters";
  return absl::OkStatus();
}

std::string Type::AddCapitalizedTypePrefix(absl::string_view input,
                                           bool is_null) const {
  if (kind() == TYPE_PROTO && !is_null) {
    // Proto types wrap their values using curly brackets, so don't need
    // to add additional parentheses.
    return absl::StrCat(CapitalizedName(), input);
  }

  return absl::StrCat(CapitalizedName(), "(", input, ")");
}

}  // namespace zetasql
