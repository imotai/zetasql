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

#include "zetasql/tools/execute_query/execute_query_tool.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "zetasql/common/options_utils.h"
#include "zetasql/parser/parse_tree.h"
#include "zetasql/parser/parser.h"
#include "zetasql/parser/parser_mode.h"
#include "zetasql/public/analyzer.h"
#include "zetasql/public/analyzer_output.h"
#include "zetasql/public/builtin_function_options.h"
#include "zetasql/public/catalog.h"
#include "zetasql/public/error_helpers.h"
#include "zetasql/public/evaluator.h"
#include "zetasql/public/evaluator_table_iterator.h"
#include "zetasql/public/function.h"
#include "zetasql/public/function_signature.h"
#include "zetasql/public/functions/like.h"
#include "zetasql/public/multi_catalog.h"
#include "zetasql/public/parse_helpers.h"
#include "zetasql/public/parse_resume_location.h"
#include "zetasql/public/parse_tokens.h"
#include "zetasql/public/simple_catalog.h"
#include "zetasql/public/simple_catalog_util.h"
#include "zetasql/public/sql_constant.h"
#include "zetasql/public/sql_formatter.h"
#include "zetasql/public/strings.h"
#include "zetasql/public/type.h"
#include "zetasql/public/types/proto_type.h"
#include "zetasql/reference_impl/statement_evaluator.h"
#include "zetasql/resolved_ast/resolved_ast.h"
#include "zetasql/resolved_ast/resolved_node.h"
#include "zetasql/resolved_ast/resolved_node_kind.pb.h"
#include "zetasql/resolved_ast/sql_builder.h"
#include "zetasql/scripting/script_executor.h"
#include "zetasql/scripting/script_segment.h"
#include "zetasql/tools/execute_query/execute_query_proto_writer.h"
#include "zetasql/tools/execute_query/execute_query_writer.h"
#include "zetasql/tools/execute_query/selectable_catalog.h"
#include "zetasql/tools/execute_query/value_as_table_adapter.h"
#include "zetasql/base/case.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/functional/any_invocable.h"
#include "zetasql/base/check.h"
#include "absl/log/log.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor_database.h"
#include "google/protobuf/message.h"
#include "re2/re2.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status_macros.h"

ABSL_FLAG(std::string, product_mode, "internal",
          "The product_mode to use in language options. Note, language_features"
          " is an orthongal way to configure language options."
          "\nValid values are:"
          "\n     'internal': supports protos, DOUBLE, signed ints, etc. "
          "\n     'external': mode used in Cloud engines");

ABSL_FLAG(std::vector<std::string>, mode, {"execute"},
          "The comma-separated tool modes to use. Valid values are:"
          "\n     'parse'   parse the parser AST"
          "\n     'unparse'  parse, then dump as sql"
          "\n     'analyze'  print the resolved AST"
          "\n     'unanalyze'  analyze, then dump as sql"
          "\n     'explain'  print the evaluator query plan"
          "\n     'execute'  actually run the query and print the result. (not"
          "                  all functionality is supported).");

ABSL_FLAG(std::string, catalog, "none",
          absl::StrCat(
              "The base catalog to use for looking up tables, etc.\nChoices:\n",
              zetasql::GetSelectableCatalogDescriptionsForFlag()));

ABSL_FLAG(zetasql::internal::EnabledAstRewrites, enabled_ast_rewrites,
          zetasql::internal::EnabledAstRewrites{
              .enabled_ast_rewrites = zetasql::internal::GetRewrites(
                  /*include_in_development=*/false,
                  /*include_default_disabled=*/true)},
          "The AST Rewrites to enable in the analyzer, format is:"
          "\n   <BASE>[,+<ADDED_OPTION>][,-<REMOVED_OPTION>]..."
          "\n Where BASE is one of:"
          "\n   'NONE': The empty set"
          "\n   'ALL': All possible rewrites, including those in "
          "development. Not recommended, in-development rewrites may produce "
          "incorrect results"
          "\n   'ALL_MINUS_DEV': (Default) All rewrites except those in "
          "development"
          "\n   'DEFAULTS': All ResolvedASTRewrite's with 'default_enabled' "
          "set. Not recommended, in-development rewrites may produce incorrect "
          "results"
          "\n   'DEFAULTS_MINUS_DEV': All rewrites with 'default_enabled' set, "
          "except those in development"
          "\n"
          "\n Enum values must be listed with 'REWRITE_' stripped"
          "\n Example:"
          "\n    --enabled_ast_rewrites='DEFAULTS,-FLATTEN,+ANONYMIZATION'"
          "\n Will enable all the default options plus ANONYMIZATION, but"
          " excluding flatten");

ABSL_FLAG(bool, fold_literal_cast, true,
          "Set the fold_literal_cast option in AnalyzerOptions");

ABSL_FLAG(std::optional<zetasql::internal::EnabledLanguageFeatures>,
          enabled_language_features, std::nullopt,
          zetasql::internal::EnabledLanguageFeatures::kFlagDescription);

ABSL_FLAG(std::string, parameters, {},
          zetasql::internal::kQueryParameterMapHelpstring);

ABSL_FLAG(bool, strict_name_resolution_mode, false,
          "Sets LanguageOptions::strict_resolution_mode.");

ABSL_FLAG(bool, evaluator_scramble_undefined_orderings, false,
          "When true, shuffle the order of rows in intermediate results that "
          "are unordered.");

ABSL_FLAG(bool, prune_unused_columns, true,
          "Sets AnalyzerOptions::prune_unused_columns.");

ABSL_FLAG(std::string, table_spec, "",
          "The table spec to use for building the ZetaSQL Catalog. This is a "
          "comma-delimited list of strings of the form <table_name>=<spec>, "
          "where <spec> is of the form:"
          "\n    binproto:<proto>:<path> - binary proto file that is "
          "represented by a value table"
          "\n    textproto:<proto>:<path> - text proto file that is "
          "represented by a value table"
          "\n    csv:<path> - csv file that is represented by a table whose "
          "string-typed column names are determined from the header row.");

ABSL_FLAG(
    std::string, descriptor_pool,
    "generated",
    "The descriptor pool to use while resolving the query. This can be:"
    "\n    'generated' - the generated pool of protos compiled into "
    "this binary"
    "\n    'none'      - no protos are included (but syntax is still "
    "supported");
// TODO: Support specifying proto files to parse.

ABSL_FLAG(std::string, output_mode, "box",
          "Format to use for query results. Available choices:"
          "\nbox - Tabular format for human consumption"
          "\njson - JSON serialization"
          "\ntextproto - Protocol buffer text format");

ABSL_FLAG(std::string, sql_mode, "query",
          "How to interpret the input sql. Available choices:"
          "\nquery"
          "\nexpression"
          "\nscript");

ABSL_FLAG(std::string, target_syntax, "standard",
          "The syntax to use when generating SQL from the resolved AST. "
          "Available choices:"
          "\nstandard - The standard syntax with nested subqueries"
          "\npipe - The pipe syntax with flattened subqueries");

ABSL_FLAG(
    int64_t, evaluator_max_value_byte_size, -1 /* sentinel for unset*/,
    R"(Limit on the maximum number of in-memory bytes used by an individual Value
  that is constructed during evaluation. This bound applies to all Value
  types, including variable-sized types like STRING, BYTES, ARRAY, and
  STRUCT. Exceeding this limit results in an error. See the implementation of
  Value::physical_byte_size for more details.)");

ABSL_FLAG(
    int64_t, evaluator_max_intermediate_byte_size, -1 /* sentinel for unset*/,
    R"(The limit on the maximum number of in-memory bytes that can be used for
  storing accumulated rows (e.g., during an ORDER BY query). Exceeding this
  limit results in an error.)");

ABSL_FLAG(
    int64_t, max_statements_to_execute, 1000,
    "The limit on number of statements allowed for execution. Post this "
    "limit, script is considered to have infinite loop and returned error.");

namespace zetasql {

namespace {
using ToolMode = ExecuteQueryConfig::ToolMode;
using SqlMode = ExecuteQueryConfig::SqlMode;

struct SqlModeMap {
  constexpr SqlModeMap(absl::string_view mode_name, SqlMode mode_enum)
      : mode_name(mode_name), mode_enum(mode_enum) {}

  absl::string_view mode_name;
  SqlMode mode_enum;
};

constexpr auto kSqlModeMap = std::array<SqlModeMap, 3>({{
    SqlModeMap("query", SqlMode::kQuery),
    SqlModeMap("expression", SqlMode::kExpression),
    SqlModeMap("script", SqlMode::kScript),
}});

constexpr parser::MacroExpansionMode kMacroExpansionMode =
    parser::MacroExpansionMode::kStrict;

// This callback is used to store the results of the statements executed.
// Current implementation stores the vector of all the results.
// Mapping of query/statement to result shall be done when needed.
// Note: Results of all the queries are stored. Even the result of the table
// expression in FOR loop.
class EvaluatorCallback : public StatementEvaluatorCallback {
 public:
  explicit EvaluatorCallback(ExecuteQueryWriter& writer,
                             ExecuteQueryConfig& config)
      : StatementEvaluatorCallback(/*bytes_per_iterator=*/100),
        writer_(writer),
        config_(config) {}

  // Result is ignored if status is not ok.
  // Caller is expected to stop the execution after an error is encountered.
  // As the caller gets the error status, errors is not stored in this callback.
  void OnStatementResult(
      const ScriptSegment& segment, const ResolvedStatement* resolved_stmt,
      const absl::StatusOr<Value>& status_or_result) override {
    absl::string_view segment_text = segment.GetSegmentText();
    if (segment_text.empty() || resolved_stmt == nullptr) {
      // The callback is also called for declarations etc
      return;
    }

    absl::Status status =
        ProcessResult(segment_text, resolved_stmt, status_or_result);
    if (!status.ok()) {
      ABSL_LOG(ERROR) << "Failed to process result for statement: " << segment_text
                 << " status: " << status;
    }
  }

 private:
  absl::Status ProcessResult(absl::string_view segment_text,
                             const ResolvedStatement* resolved_stmt,
                             const absl::StatusOr<Value>& result) {
    // In script mode, we want to always print the statement text, so we always
    // call StartStatement() with is_first as false.
    ZETASQL_RETURN_IF_ERROR(writer_.StartStatement(/*is_first=*/false));
    (void)writer_.statement_text(segment_text);
    if (config_.tool_modes().contains(ToolMode::kResolve)) {
      ZETASQL_RETURN_IF_ERROR(writer_.resolved(*resolved_stmt));
    }

    if (result.ok()) {
      const Value& value = result.value();
      // DDL statements don't return a value.
      if (!value.has_content()) {
        return absl::OkStatus();
      }
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<ValueAsTableAdapter> adapter,
                       ValueAsTableAdapter::Create(value));
      ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<EvaluatorTableIterator> iter,
                       adapter->CreateEvaluatorTableIterator());
      ZETASQL_RETURN_IF_ERROR(writer_.executed(*resolved_stmt, std::move(iter)));
    }

    return absl::OkStatus();
  }

 private:
  ExecuteQueryWriter& writer_;
  ExecuteQueryConfig& config_;
};

}  // namespace

std::optional<ToolMode> ExecuteQueryConfig::parse_tool_mode(
    absl::string_view mode) {
  static const auto* kToolModeMap =
      new absl::flat_hash_map<absl::string_view, ToolMode>({
          {"parse", ToolMode::kParse},
          {"parser", ToolMode::kParse},
          {"unparse", ToolMode::kUnparse},
          {"unparser", ToolMode::kUnparse},
          {"resolve", ToolMode::kResolve},
          {"resolver", ToolMode::kResolve},
          {"analyze", ToolMode::kResolve},
          {"analyzer", ToolMode::kResolve},
          {"sql_builder", ToolMode::kUnAnalyze},
          {"sqlbuilder", ToolMode::kUnAnalyze},
          {"unanalyze", ToolMode::kUnAnalyze},
          {"unanalyzer", ToolMode::kUnAnalyze},
          {"unresolve", ToolMode::kUnAnalyze},
          {"unresolver", ToolMode::kUnAnalyze},
          {"explain", ToolMode::kExplain},
          {"execute", ToolMode::kExecute},
      });

  std::string mode_lower{absl::AsciiStrToLower(mode)};
  auto it = kToolModeMap->find(mode_lower);
  if (it == kToolModeMap->end()) {
    return std::nullopt;
  }
  return it->second;
}

absl::string_view ExecuteQueryConfig::tool_mode_name(ToolMode tool_mode) {
  static const auto* kToolModeNames =
      new absl::flat_hash_map<ToolMode, absl::string_view>({
          {ToolMode::kParse, "parse"},
          {ToolMode::kUnparse, "unparse"},
          {ToolMode::kResolve, "analyze"},
          {ToolMode::kUnAnalyze, "unanalyze"},
          {ToolMode::kExplain, "explain"},
          {ToolMode::kExecute, "execute"},
      });

  auto it = kToolModeNames->find(tool_mode);
  ABSL_CHECK(it != kToolModeNames->end())
      << "Unknown tool mode: " << static_cast<int>(tool_mode);
  return it->second;
}

// Returns the sql mode if the mode string matches one of the sql modes.
std::optional<SqlMode> ExecuteQueryConfig::parse_sql_mode(
    absl::string_view mode) {
  std::string mode_lower{absl::AsciiStrToLower(mode)};
  for (const SqlModeMap& sql_mode : kSqlModeMap) {
    if (mode_lower == sql_mode.mode_name) {
      return sql_mode.mode_enum;
    }
  }
  return std::nullopt;
}

// Returns the name of the sql mode.
absl::string_view ExecuteQueryConfig::sql_mode_name(SqlMode sql_mode_enum) {
  for (const SqlModeMap& sql_mode : kSqlModeMap) {
    if (sql_mode.mode_enum == sql_mode_enum) {
      return sql_mode.mode_name;
    }
  }
  ABSL_LOG(FATAL) << "Unknown sql mode: " << static_cast<int>(sql_mode_enum);
}

absl::Status SetToolModeFromFlags(ExecuteQueryConfig& config) {
  const std::vector<std::string> mode = absl::GetFlag(FLAGS_mode);
  const absl::flat_hash_set<absl::string_view> modes(mode.begin(), mode.end());

  config.clear_tool_modes();
  if (modes.empty()) {
    config.add_tool_mode(ToolMode::kExecute);
    return absl::OkStatus();
  }

  for (const auto& mode : modes) {
    std::optional<ToolMode> tool_mode =
        ExecuteQueryConfig::parse_tool_mode(mode);
    if (tool_mode.has_value()) {
      config.add_tool_mode(*tool_mode);
    } else {
      return zetasql_base::InvalidArgumentErrorBuilder()
             << "Invalid --mode: '" << mode << "'";
    }
  }

  return absl::OkStatus();
}

absl::Status SetSqlModeFromFlags(ExecuteQueryConfig& config) {
  const std::string sql_mode_str = absl::GetFlag(FLAGS_sql_mode);
  std::optional<SqlMode> sql_mode =
      ExecuteQueryConfig::parse_sql_mode(sql_mode_str);
  if (sql_mode.has_value()) {
    config.set_sql_mode(*sql_mode);
    return absl::OkStatus();
  }
  return zetasql_base::InvalidArgumentErrorBuilder()
         << "Invalid --sql_mode: '" << sql_mode_str << "'";
}

absl::Status SetTargetSyntaxModeFromFlags(ExecuteQueryConfig& config) {
  return config.SetTargetSyntaxModeFromString(
      absl::GetFlag(FLAGS_target_syntax));
}

std::optional<SQLBuilder::TargetSyntaxMode>
ExecuteQueryConfig::parse_target_syntax_mode(absl::string_view mode) {
  if (mode == "standard") {
    return SQLBuilder::TargetSyntaxMode::kStandard;
  } else if (mode == "pipe") {
    return SQLBuilder::TargetSyntaxMode::kPipe;
  }
  return std::nullopt;
}

absl::string_view ExecuteQueryConfig::target_syntax_mode_name(
    SQLBuilder::TargetSyntaxMode target_syntax_mode) {
  switch (target_syntax_mode) {
    case SQLBuilder::TargetSyntaxMode::kStandard:
      return "standard";
    case SQLBuilder::TargetSyntaxMode::kPipe:
      return "pipe";
  }
  ABSL_LOG(FATAL) << "Unknown target syntax mode: "
             << static_cast<int>(target_syntax_mode);
}

absl::Status ExecuteQueryConfig::SetTargetSyntaxModeFromString(
    absl::string_view target_syntax_mode_str) {
  std::optional<SQLBuilder::TargetSyntaxMode> target_syntax_mode =
      parse_target_syntax_mode(target_syntax_mode_str);
  if (target_syntax_mode.has_value()) {
    set_target_syntax_mode(*target_syntax_mode);
    return absl::OkStatus();
  }

  return zetasql_base::InvalidArgumentErrorBuilder()
         << "Invalid --target_syntax_mode: '" << target_syntax_mode_str << "'";
}

static absl::Status SetFoldLiteralCastFromFlags(ExecuteQueryConfig& config) {
  config.mutable_analyzer_options().set_fold_literal_cast(
      absl::GetFlag(FLAGS_fold_literal_cast));
  return absl::OkStatus();
}

static absl::Status SetRewritersFromFlags(ExecuteQueryConfig& config) {
  config.mutable_analyzer_options().set_enabled_rewrites(
      absl::GetFlag(FLAGS_enabled_ast_rewrites).enabled_ast_rewrites);
  return absl::OkStatus();
}

static absl::Status SetLanguageFeaturesFromFlags(ExecuteQueryConfig& config) {
  std::optional<internal::EnabledLanguageFeatures> features =
      absl::GetFlag(FLAGS_enabled_language_features);
  if (features.has_value()) {
    config.mutable_analyzer_options()
        .mutable_language()
        ->SetEnabledLanguageFeatures(
            {features->enabled_language_features.begin(),
             features->enabled_language_features.end()});
  }
  return absl::OkStatus();
}

static absl::Status SetProductModeFromFlags(ExecuteQueryConfig& config) {
  std::string product_mode =
      absl::AsciiStrToLower(absl::GetFlag(FLAGS_product_mode));
  if (product_mode == "internal") {
    config.mutable_analyzer_options().mutable_language()->set_product_mode(
        PRODUCT_INTERNAL);
    return absl::OkStatus();
  } else if (product_mode == "external") {
    config.mutable_analyzer_options().mutable_language()->set_product_mode(
        PRODUCT_EXTERNAL);
    return absl::OkStatus();
  }
  return zetasql_base::InvalidArgumentErrorBuilder()
         << "Invalid --product_mode:'" << product_mode << "'";
}

static absl::Status SetNameResolutionModeFromFlags(ExecuteQueryConfig& config) {
  config.mutable_analyzer_options()
      .mutable_language()
      ->set_name_resolution_mode(
          absl::GetFlag(FLAGS_strict_name_resolution_mode)
              ? NAME_RESOLUTION_STRICT
              : NAME_RESOLUTION_DEFAULT);

  return absl::OkStatus();
}

absl::Status SetDescriptorPoolFromFlags(ExecuteQueryConfig& config) {
  const std::string pool = absl::GetFlag(FLAGS_descriptor_pool);

  if (pool == "none") {
    // Do nothing
    return absl::OkStatus();
  } else if (pool == "generated") {
    config.SetDescriptorPool(google::protobuf::DescriptorPool::generated_pool());
    return absl::OkStatus();
  } else {
    return absl::Status(absl::StatusCode::kInvalidArgument,
                        "--descriptor_pool flag must be one of: none, generated"
    );
  }
}

static absl::StatusOr<const ProtoType*> GetProtoType(
    ExecuteQueryConfig& config, absl::string_view proto_name) {
  const zetasql::Type* type = nullptr;
  if (!config.catalog()->FindType({std::string(proto_name)}, &type).ok() ||
      type == nullptr) {
    return zetasql_base::NotFoundErrorBuilder()
           << "Unknown protocol buffer message: '" << proto_name << "'";
  }
  ZETASQL_RET_CHECK(type->IsProto());
  return type->AsProto();
}

static absl::StatusOr<std::unique_ptr<const Table>> MakeTableFromTableSpec(
    absl::string_view table_spec, ExecuteQueryConfig& config) {
  std::vector<absl::string_view> table_spec_parts =
      absl::StrSplit(table_spec, absl::MaxSplits('=', 1));
  if (table_spec_parts.size() != 2) {
    return zetasql_base::InvalidArgumentErrorBuilder()
           << "Invalid table specification: " << table_spec;
  }
  absl::string_view table_name = table_spec_parts[0];
  absl::string_view spec = table_spec_parts[1];

  std::vector<std::string> spec_parts = absl::StrSplit(spec, ':');
  if (spec_parts.empty()) {
    return zetasql_base::InvalidArgumentErrorBuilder()
           << "Invalid empty specification for table " << table_name;
  }
  absl::string_view format = spec_parts[0];
  if (format == "csv") {
    if (spec_parts.size() != 2) {
      return zetasql_base::InvalidArgumentErrorBuilder()
             << "Invalid specification for csv table " << table_name << ": "
             << table_spec;
    }
    absl::string_view path = spec_parts[1];
    return MakeTableFromCsvFile(table_name, path);
  } else if (format == "binproto") {
    if (spec_parts.size() != 3) {
      return zetasql_base::InvalidArgumentErrorBuilder()
             << "Invalid specification for table " << table_name << ": "
             << table_spec;
    }
    absl::string_view proto_name = spec_parts[1];
    absl::string_view path = spec_parts[2];

    ZETASQL_ASSIGN_OR_RETURN(const ProtoType* record_type,
                     GetProtoType(config, proto_name));
    return MakeTableFromBinaryProtoFile(table_name, path, record_type);
  } else if (format == "textproto") {
    if (spec_parts.size() != 3) {
      return zetasql_base::InvalidArgumentErrorBuilder()
             << "Invalid specification for table " << table_name << ": "
             << table_spec;
    }
    absl::string_view proto_name = spec_parts[1];
    absl::string_view path = spec_parts[2];

    ZETASQL_ASSIGN_OR_RETURN(const ProtoType* record_type,
                     GetProtoType(config, proto_name));
    return MakeTableFromTextProtoFile(table_name, path, record_type);
  } else {
    return zetasql_base::InvalidArgumentErrorBuilder()
           << "Unknown format " << format << " for table " << table_name;
  }
}

absl::Status ExecuteQueryConfig::SetCatalogFromString(absl::string_view value) {
  ZETASQL_ASSIGN_OR_RETURN(SelectableCatalog * selectable_catalog,
                   FindSelectableCatalog(value));

  ZETASQL_ASSIGN_OR_RETURN(Catalog * catalog, selectable_catalog->GetCatalog(
                                          analyzer_options().language()));
  SetBaseCatalog(catalog);

  return absl::OkStatus();
}

absl::Status AddTablesFromFlags(ExecuteQueryConfig& config) {
  std::vector<std::string> table_specs =
      absl::StrSplit(absl::GetFlag(FLAGS_table_spec), ',', absl::SkipEmpty());

  for (const std::string& table_spec : table_specs) {
    ZETASQL_ASSIGN_OR_RETURN(auto table, MakeTableFromTableSpec(table_spec, config));
    config.wrapper_catalog()->AddOwnedTable(std::move(table));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<ExecuteQueryWriter>> MakeWriterFromFlags(
    const ExecuteQueryConfig& config, std::ostream& output) {
  const std::string mode = absl::GetFlag(FLAGS_output_mode);

  if (mode.empty()) {
    return zetasql_base::InvalidArgumentErrorBuilder() << "Must specify --output_mode";
  }

  if (mode == "box") {
    return std::make_unique<ExecuteQueryStreamWriter>(output);
  }

  std::function<absl::Status(const google::protobuf::Message& msg, std::ostream&)>
      proto_writer_func;

  if (mode == "json") {
    proto_writer_func = &ExecuteQueryWriteJson;
  } else if (mode == "textproto") {
    proto_writer_func = &ExecuteQueryWriteTextproto;
  } else {
    return zetasql_base::InvalidArgumentErrorBuilder()
           << "Unknown output mode " << mode;
  }

  ZETASQL_RET_CHECK(proto_writer_func != nullptr);

  const google::protobuf::DescriptorPool* pool = config.descriptor_pool();

  ZETASQL_RET_CHECK_NE(pool, nullptr);

  return std::make_unique<ExecuteQueryStreamProtobufWriter>(
      pool, [proto_writer_func, &output](const google::protobuf::Message& msg) {
        return proto_writer_func(msg, output);
      });
}

absl::Status SetLanguageOptionsFromFlags(ExecuteQueryConfig& config) {
  ZETASQL_RETURN_IF_ERROR(SetProductModeFromFlags(config));
  ZETASQL_RETURN_IF_ERROR(SetNameResolutionModeFromFlags(config));
  return SetLanguageFeaturesFromFlags(config);
}

absl::Status SetAnalyzerOptionsFromFlags(ExecuteQueryConfig& config) {
  ZETASQL_RETURN_IF_ERROR(SetFoldLiteralCastFromFlags(config));
  return SetRewritersFromFlags(config);
}

absl::Status SetEvaluatorOptionsFromFlags(ExecuteQueryConfig& config) {
  if (int64_t val = absl::GetFlag(FLAGS_evaluator_max_value_byte_size);
      val != -1) {
    config.mutable_evaluator_options().max_value_byte_size = val;
  }
  if (int64_t val = absl::GetFlag(FLAGS_evaluator_max_intermediate_byte_size);
      val != -1) {
    config.mutable_evaluator_options().max_intermediate_byte_size = val;
  }
  config.mutable_evaluator_options().scramble_undefined_orderings =
      absl::GetFlag(FLAGS_evaluator_scramble_undefined_orderings);
  return absl::OkStatus();
}

absl::Status SetQueryParametersFromFlags(ExecuteQueryConfig& config) {
  ParameterValueMap parameters;
  std::string err;
  if (!internal::ParseQueryParameterFlag(absl::GetFlag(FLAGS_parameters),
                                         config.analyzer_options(),
                                         config.catalog(), &parameters, &err)) {
    return absl::InvalidArgumentError(err);
  }
  for (const auto& [name, value] : parameters) {
    ZETASQL_RETURN_IF_ERROR(config.mutable_analyzer_options().AddQueryParameter(
        name, value.type()));
  }

  config.mutable_query_parameter_values() = std::move(parameters);
  return absl::OkStatus();
}

absl::Status InitializeExecuteQueryConfig(ExecuteQueryConfig& config) {
  // Prefer a default of maximum (this will be overwritten if an explicit
  // flag value is passed).
  config.mutable_analyzer_options()
      .mutable_language()
      ->EnableMaximumLanguageFeatures();
  config.mutable_analyzer_options()
      .mutable_language()
      ->SetSupportsAllStatementKinds();
  config.mutable_analyzer_options()
      .mutable_language()
      ->EnableAllReservableKeywords();

  // Used to pretty print error message in tandem with
  // ExecuteQueryLoopPrintErrorHandler.
  config.mutable_analyzer_options().set_error_message_mode(
      ERROR_MESSAGE_WITH_PAYLOAD);

  config.mutable_analyzer_options().set_prune_unused_columns(
      absl::GetFlag(FLAGS_prune_unused_columns));

  ZETASQL_RETURN_IF_ERROR(SetDescriptorPoolFromFlags(config));
  ZETASQL_RETURN_IF_ERROR(SetToolModeFromFlags(config));
  ZETASQL_RETURN_IF_ERROR(SetSqlModeFromFlags(config));
  ZETASQL_RETURN_IF_ERROR(SetTargetSyntaxModeFromFlags(config));
  ZETASQL_RETURN_IF_ERROR(config.SetCatalogFromString(absl::GetFlag(FLAGS_catalog)));
  ZETASQL_RETURN_IF_ERROR(SetLanguageOptionsFromFlags(config));
  ZETASQL_RETURN_IF_ERROR(SetAnalyzerOptionsFromFlags(config));
  ZETASQL_RETURN_IF_ERROR(SetEvaluatorOptionsFromFlags(config));
  ZETASQL_RETURN_IF_ERROR(AddTablesFromFlags(config));

  ZETASQL_RETURN_IF_ERROR(config.builtins_catalog()->AddBuiltinFunctionsAndTypes(
      BuiltinFunctionOptions(config.analyzer_options().language())));
  ZETASQL_RETURN_IF_ERROR(SetQueryParametersFromFlags(config));

  return absl::OkStatus();
}

ExecuteQueryConfig::ExecuteQueryConfig()
    : builtins_catalog_(std::make_unique<SimpleCatalog>("")),
      wrapper_catalog_("") {
  SetBaseCatalog(nullptr);  // This also sets up the MultiCatalog.
}

void ExecuteQueryConfig::SetBaseCatalog(Catalog* catalog) {
  base_catalog_ = catalog;
  RebuildMultiCatalog();
}

void ExecuteQueryConfig::SetBuiltinsCatalogFromLanguageOptions(
    const LanguageOptions& language_options) {
  builtins_catalog_ = std::make_unique<SimpleCatalog>("");
  ZETASQL_CHECK_OK(builtins_catalog_->AddBuiltinFunctionsAndTypes(
      BuiltinFunctionOptions(language_options)));
  RebuildMultiCatalog();
}

void ExecuteQueryConfig::RebuildMultiCatalog() {
  std::vector<Catalog*> catalogs;
  catalogs.push_back(&wrapper_catalog_);
  if (base_catalog_ != nullptr) {
    catalogs.push_back(base_catalog_);
  }
  ABSL_CHECK_NE(builtins_catalog_.get(), nullptr);
  catalogs.push_back(builtins_catalog_.get());

  // The only case where this fails is if one of the catalogs is nullptr,
  // which can't happen here.
  ZETASQL_CHECK_OK(MultiCatalog::Create("", catalogs, &catalog_));
}

void ExecuteQueryConfig::SetDescriptorPool(const google::protobuf::DescriptorPool* pool) {
  ABSL_CHECK(descriptor_pool_ == nullptr) << __func__ << " can only be called once";
  owned_descriptor_pool_.reset();
  descriptor_pool_ = pool;
  wrapper_catalog_.SetDescriptorPool(pool);
}

void ExecuteQueryConfig::SetOwnedDescriptorPool(
    std::unique_ptr<const google::protobuf::DescriptorPool> pool) {
  ABSL_CHECK(descriptor_pool_ == nullptr) << __func__ << " can only be called once";
  owned_descriptor_pool_ = std::move(pool);
  descriptor_pool_ = owned_descriptor_pool_.get();
  wrapper_catalog_.SetDescriptorPool(descriptor_pool_);
}

void ExecuteQueryConfig::SetOwnedDescriptorDatabase(
    std::unique_ptr<google::protobuf::DescriptorDatabase> db) {
  ABSL_CHECK(descriptor_db_ == nullptr) << __func__ << " can only be called once";

  // The descriptor database given to the pool needs to be owned locally.
  descriptor_db_ = std::move(db);

  SetOwnedDescriptorPool(
      std::make_unique<const google::protobuf::DescriptorPool>(descriptor_db_.get()));
}

static absl::StatusOr<bool> RegisterMacro(absl::string_view sql,
                                          ExecuteQueryConfig& config,
                                          ExecuteQueryWriter& writer) {
  // TODO: Hack: we did not implement
  // ResolvedDefineMacroStatement yet, so for now we short-circuit into
  // updating the macro catalog.
  std::unique_ptr<ParserOutput> parser_output;
  absl::Status parse_stmt = ParseStatement(
      sql,
      ParserOptions(config.analyzer_options().language(), kMacroExpansionMode,
                    &config.macro_catalog()),
      &parser_output);
  if (parse_stmt.ok() &&
      parser_output->statement()->Is<ASTDefineMacroStatement>()) {
    config.mutable_macro_sources().push_back(std::string(sql));
    auto define_macro_statement =
        parser_output->statement()->GetAsOrNull<ASTDefineMacroStatement>();
    ZETASQL_RETURN_IF_ERROR(config.mutable_macro_catalog().RegisterMacro(
        {.source_text = config.macro_sources().back(),
         .location = define_macro_statement->location(),
         .name_location = define_macro_statement->name()->location(),
         .body_location = define_macro_statement->body()->location()}));

    std::string macro_name = define_macro_statement->name()->GetAsString();
    ZETASQL_RETURN_IF_ERROR(
        writer.log(absl::StrFormat("Macro registered: %s", macro_name)));
    return true;
  }

  return false;
}

static absl::StatusOr<const ASTNode*> ParseSql(
    absl::string_view sql, const ExecuteQueryConfig& config,
    ParseResumeLocation* parse_resume_location, bool* at_end_of_input,
    std::unique_ptr<ParserOutput>* parser_output) {
  ParserOptions parser_options(config.analyzer_options().language(),
                               kMacroExpansionMode, &config.macro_catalog());
  const ASTNode* root = nullptr;
  switch (config.sql_mode()) {
    case SqlMode::kQuery: {
      std::string input(sql);
      ZETASQL_RETURN_IF_ERROR(ParseNextStatement(parse_resume_location, parser_options,
                                         parser_output, at_end_of_input));
      ZETASQL_RET_CHECK(at_end_of_input);
      root = (*parser_output)->statement();
      break;
    }
    case SqlMode::kExpression: {
      ZETASQL_RETURN_IF_ERROR(ParseExpression(sql, parser_options, parser_output));
      root = (*parser_output)->expression();
      // Expressions are always just a single parsable item.
      *at_end_of_input = true;
      break;
    }
    case SqlMode::kScript: {
      ZETASQL_RET_CHECK(at_end_of_input == nullptr);
      ZETASQL_RETURN_IF_ERROR(ParseScript(
          sql, parser_options,
          config.analyzer_options().error_message_options(), parser_output));
      root = (*parser_output)->script();
      break;
    }
  }
  ZETASQL_RET_CHECK_NE(root, nullptr);
  return root;
}

static absl::Status WriteParsedAndOrUnparsedAst(
    const ASTNode* root, const ExecuteQueryConfig& config,
    ExecuteQueryWriter& writer) {
  if (config.has_tool_mode(ToolMode::kParse)) {
    // Note, ASTNode is not public, and therefore cannot be part of the
    // public interface, thus, we can only return the string.
    ZETASQL_RETURN_IF_ERROR(writer.parsed(root->DebugString()));
  }
  if (config.has_tool_mode(ToolMode::kUnparse)) {
    ZETASQL_RETURN_IF_ERROR(writer.unparsed(Unparse(root)));
  }

  return absl::OkStatus();
}

static bool IsAnalysisRequired(const ExecuteQueryConfig& config) {
  return config.has_tool_mode(ToolMode::kResolve) ||
         config.has_tool_mode(ToolMode::kUnAnalyze) ||
         config.has_tool_mode(ToolMode::kExplain) ||
         config.has_tool_mode(ToolMode::kExecute);
}

static absl::StatusOr<const ResolvedNode*> AnalyzeSql(
    absl::string_view sql, const ASTNode* ast, ExecuteQueryConfig& config,
    ExecuteQueryWriter& writer,
    std::unique_ptr<const AnalyzerOutput>* analyzer_output) {
  const ResolvedNode* resolved_node = nullptr;
  switch (config.sql_mode()) {
    case SqlMode::kQuery: {
      ZETASQL_RETURN_IF_ERROR(AnalyzeStatementFromParserAST(
          *ast->GetAsOrDie<ASTStatement>(), config.analyzer_options(), sql,
          config.catalog(), config.type_factory(), analyzer_output));
      resolved_node = (*analyzer_output)->resolved_statement();
      break;
    }
    case SqlMode::kExpression: {
      ZETASQL_RETURN_IF_ERROR(AnalyzeExpressionFromParserAST(
          *ast->GetAsOrDie<ASTExpression>(), config.analyzer_options(), sql,
          config.type_factory(), config.catalog(), analyzer_output));
      resolved_node = (*analyzer_output)->resolved_expr();
      break;
    }
    case SqlMode::kScript: {
      // AnalyzeSql is never really called in script mode. But just in case...
      return absl::UnimplementedError(
          "Analysis is not supported in \"script\" sql_mode.");
    }
  }

  if (config.has_tool_mode(ToolMode::kResolve)) {
    ZETASQL_RET_CHECK_NE(resolved_node, nullptr);
    ZETASQL_RETURN_IF_ERROR(writer.resolved(*resolved_node));

    if (!(*analyzer_output)->deprecation_warnings().empty()) {
      ZETASQL_RETURN_IF_ERROR(writer.log("Deprecation warnings:"));
      for (const auto& warning : (*analyzer_output)->deprecation_warnings()) {
        ZETASQL_RETURN_IF_ERROR(writer.log(warning.message()));
      }
    }
  }

  return resolved_node;
}

static absl::Status UnanalyzeQuery(const ResolvedNode* resolved_node,
                                   ExecuteQueryConfig& config,
                                   ExecuteQueryWriter& writer) {
  SQLBuilder::SQLBuilderOptions sql_builder_options;
  sql_builder_options.language_options = config.analyzer_options().language();
  sql_builder_options.catalog = config.catalog();
  sql_builder_options.target_syntax_mode = config.target_syntax_mode();
  SQLBuilder builder(sql_builder_options);
  ZETASQL_RETURN_IF_ERROR(builder.Process(*resolved_node));

  std::string formatted_sql;
  ZETASQL_RETURN_IF_ERROR(FormatSql(builder.sql(), &formatted_sql));
  formatted_sql.append(1, '\n');
  return writer.unanalyze(formatted_sql);
}

// If `resolved_node` is a DDL CREATE statement, try to add the created
// object to the catalog.  We do this even if not in execute mode since later
// statements may depend on these objects for analysis.
static absl::Status HandleDDL(
    const ResolvedNode* resolved_node, ExecuteQueryConfig& config,
    ExecuteQueryWriter& writer, std::unique_ptr<ParserOutput>* parser_output,
    std::unique_ptr<const AnalyzerOutput>* analyzer_output,
    bool* executed_as_ddl) {
  const bool is_ctas = resolved_node->Is<ResolvedCreateTableAsSelectStmt>();

  switch (resolved_node->node_kind()) {
    case RESOLVED_CREATE_FUNCTION_STMT: {
      const auto* stmt = resolved_node->GetAs<ResolvedCreateFunctionStmt>();
      ZETASQL_ASSIGN_OR_RETURN(auto function, MakeFunctionFromCreateFunction(
                                          *stmt, /*function_options=*/nullptr));

      if (!config.wrapper_catalog()->AddOwnedFunctionIfNotPresent(&function)) {
        return zetasql_base::InvalidArgumentErrorBuilder() << "Function already exists";
      }
      ZETASQL_RETURN_IF_ERROR(writer.log("Function registered."));
      break;
    }
    case RESOLVED_CREATE_TABLE_FUNCTION_STMT: {
      const ResolvedCreateTableFunctionStmt* stmt =
          resolved_node->GetAs<ResolvedCreateTableFunctionStmt>();
      stmt->create_scope();  // Mark accessed so TEMP can be ignored.
      ZETASQL_ASSIGN_OR_RETURN(auto tvf, MakeTVFFromCreateTableFunction(*stmt));

      if (!config.wrapper_catalog()->AddOwnedTableValuedFunctionIfNotPresent(
              &tvf)) {
        return zetasql_base::InvalidArgumentErrorBuilder() << "TVF already exists";
      }
      ZETASQL_RETURN_IF_ERROR(writer.log("TVF registered."));
      break;
    }
    case RESOLVED_CREATE_TABLE_AS_SELECT_STMT:
    case RESOLVED_CREATE_TABLE_STMT: {
      const ResolvedCreateTableStmtBase* stmt =
          resolved_node->GetAs<ResolvedCreateTableStmtBase>();
      stmt->create_scope();  // Mark accessed so TEMP can be ignored.

      if (is_ctas) {
        // Mark query-related fields accessed so we don't get an error.
        // We should undo this if we start to support executing CTAS.
        const auto* ctas = stmt->GetAs<ResolvedCreateTableAsSelectStmt>();
        for (auto& col : ctas->output_column_list()) {
          col->MarkFieldsAccessed();
        }
        ctas->query()->MarkFieldsAccessed();
      }

      ZETASQL_ASSIGN_OR_RETURN(auto table, MakeTableFromCreateTable(*stmt));

      if (!is_ctas) {
        // The table will return zero rows when queried.
        table->SetContents({});
      } else {
        // Execution will fail scanning tables created with CTAS because there's
        // no data. To fix this, we could return the SimpleTable*, pass it to
        // ExplainAndOrExecuteSql, and have that run the contained query and
        // capture its result into this table.
      }

      const std::string name = table->Name();
      if (!config.wrapper_catalog()->AddOwnedTableIfNotPresent(
              name, std::move(table))) {
        return zetasql_base::InvalidArgumentErrorBuilder() << "Table already exists";
      }
      ZETASQL_RETURN_IF_ERROR(writer.log("Table registered."));
      break;
    }
    case RESOLVED_CREATE_CONSTANT_STMT: {
      const auto* stmt = resolved_node->GetAs<ResolvedCreateConstantStmt>();
      ZETASQL_ASSIGN_OR_RETURN(auto owned_constant,
                       MakeConstantFromCreateConstant(*stmt));
      SQLConstant* sql_constant = owned_constant.get();

      if (!config.wrapper_catalog()->AddOwnedConstantIfNotPresent(
              std::move(owned_constant))) {
        return zetasql_base::InvalidArgumentErrorBuilder() << "Constant already exists";
      }
      ZETASQL_RETURN_IF_ERROR(writer.log("Constant registered."));

      if (config.has_tool_mode(ToolMode::kExecute)) {
        PreparedExpression expression{sql_constant->constant_expression(),
                                      config.evaluator_options()};
        ZETASQL_RETURN_IF_ERROR(
            expression.Prepare(config.analyzer_options(), config.catalog()));

        PreparedExpressionBase::ExpressionOptions expression_options;
        expression_options.parameters = config.query_parameter_values();
        ZETASQL_ASSIGN_OR_RETURN(Value value, expression.ExecuteAfterPrepare(
                                          std::move(expression_options)));

        ZETASQL_RETURN_IF_ERROR(sql_constant->SetEvaluationResult(value));
        ZETASQL_RETURN_IF_ERROR(writer.log("Constant evaluated."));
      }
      break;
    }
    default:
      // Not a DDL statement so we don't need AddFunctionArtifacts below.
      return absl::OkStatus();
  }

  // Common code after handling any DDL CREATE successfully.
  config.AddArtifacts(std::move(*parser_output), std::move(*analyzer_output));
  *executed_as_ddl = !is_ctas;

  return absl::OkStatus();
}

static absl::Status ExplainAndOrExecuteSql(const ResolvedNode* resolved_node,
                                           ExecuteQueryConfig& config,
                                           ExecuteQueryWriter& writer,
                                           absl::string_view sql) {
  switch (config.sql_mode()) {
    case SqlMode::kQuery: {
      ZETASQL_RET_CHECK_EQ(resolved_node->node_kind(), RESOLVED_QUERY_STMT);

      PreparedQuery query{resolved_node->GetAs<ResolvedQueryStmt>(),
                          config.evaluator_options()};
      ZETASQL_RETURN_IF_ERROR(
          query.Prepare(config.analyzer_options(), config.catalog()));

      if (config.has_tool_mode(ToolMode::kExplain)) {
        ZETASQL_ASSIGN_OR_RETURN(std::string explain, query.ExplainAfterPrepare());
        explain.append(1, '\n');
        ZETASQL_RETURN_IF_ERROR(writer.explained(*resolved_node, explain));
      }

      if (config.has_tool_mode(ToolMode::kExecute)) {
        ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<EvaluatorTableIterator> iter,
                         query.ExecuteAfterPrepare(
                             {.parameters = config.query_parameter_values()}));
        ZETASQL_RETURN_IF_ERROR(writer.executed(*resolved_node, std::move(iter)));
      }

      return absl::OkStatus();
    }
    case SqlMode::kExpression: {
      ZETASQL_RET_CHECK(resolved_node->IsExpression());

      // Note there is similar expression evaluation code in the handling
      // for RESOLVED_CREATE_CONSTANT_STMT.  Keep them in sync.
      // If the code gets longer, extract common methods.
      PreparedExpression expression{resolved_node->GetAs<ResolvedExpr>(),
                                    config.evaluator_options()};
      ZETASQL_RETURN_IF_ERROR(
          expression.Prepare(config.analyzer_options(), config.catalog()));

      if (config.has_tool_mode(ToolMode::kExplain)) {
        ZETASQL_ASSIGN_OR_RETURN(const std::string explain,
                         expression.ExplainAfterPrepare());
        ZETASQL_RETURN_IF_ERROR(writer.explained(*resolved_node, explain));
      }

      if (config.has_tool_mode(ToolMode::kExecute)) {
        PreparedExpressionBase::ExpressionOptions expression_options;
        expression_options.parameters = config.query_parameter_values();
        ZETASQL_ASSIGN_OR_RETURN(Value value, expression.ExecuteAfterPrepare(
                                          std::move(expression_options)));
        ZETASQL_RETURN_IF_ERROR(writer.ExecutedExpression(*resolved_node, value));
      }

      return absl::OkStatus();
    }
    case SqlMode::kScript: {
      ScriptExecutorOptions script_executor_options;
      script_executor_options.PopulateFromAnalyzerOptions(
          config.analyzer_options());
      EvaluatorOptions evaluator_options;
      ParameterValueList positional_parameters;
      TypeFactory type_factory;
      EvaluatorCallback callback(writer, config);

      auto evaluator = std::make_unique<StatementEvaluatorImpl>(
          config.analyzer_options(), evaluator_options, positional_parameters,
          &type_factory, config.catalog(), &callback);
      evaluator->EnableCreationOfTempObjects(config.wrapper_catalog());

      absl::StatusOr<std::unique_ptr<ScriptExecutor>> script_executor =
          ScriptExecutor::Create(sql, script_executor_options, evaluator.get());
      if (!script_executor.ok()) {
        return script_executor.status();
      }
      absl::Status status = absl::OkStatus();
      int64_t executed_statement_count = 0;
      // Breaks out of the loop if the script has reached the maximum execution
      // limit or an error is encountered. Results of the previous (successful)
      // statements are still added to the ExecuteQueryWriter.
      while (executed_statement_count <
                 absl::GetFlag(FLAGS_max_statements_to_execute) &&
             status.ok() && !(*script_executor)->IsComplete()) {
        status = (*script_executor)->ExecuteNext();
        executed_statement_count += 1;
      }

      if (executed_statement_count >=
          absl::GetFlag(FLAGS_max_statements_to_execute)) {
        return absl::InvalidArgumentError(
            absl::StrCat("Potential infinite loop in the script. Script has "
                         "reached maximum execution limit: ",
                         absl::GetFlag(FLAGS_max_statements_to_execute)));
      }
      return status;
    }
  }
}

// This is a rudimentary implementation of
//   DESCRIBE [object_type] name;
// It can print a simple description of tables, functions and TVFs.
// If object_type is one of those, it'll print objects just of that type.
// If object_type isn't requested, it'll print found objects of all types.
//
// This is implemented in execute_query rather than in the reference
// implementation because DESCRIBE is engine-defined behavior and does not
// have a specified correct answer that could be compliance tested.
static absl::Status ExecuteDescribe(const ResolvedNode* resolved_node,
                                    ExecuteQueryConfig& config,
                                    ExecuteQueryWriter& writer) {
  ZETASQL_RET_CHECK_EQ(resolved_node->node_kind(), RESOLVED_DESCRIBE_STMT);
  const ResolvedDescribeStmt* describe =
      resolved_node->GetAs<ResolvedDescribeStmt>();

  const std::string object_type =
      absl::AsciiStrToLower(describe->object_type());
  const bool find_any_object_type = object_type.empty();
  bool called_any_find = false;
  bool found_any_object = false;

  const std::vector<std::string>& name_path = describe->name_path();
  ZETASQL_RETURN_IF_ERROR(describe->CheckFieldsAccessed());

  std::ostringstream output;
  Catalog* catalog = config.catalog();
  const ProductMode product_mode =
      config.analyzer_options().language().product_mode();

  // Common block around the lookup for each object type, to handle errors and
  // track whether we found anything so far.
  // Call it like
  //   IF_FOUND(catalog->FindObject(...)) {
  //     // handle the object
  //   }
#define IF_FOUND(FindCall)                                 \
  absl::Status find_status = (FindCall);                   \
  called_any_find = true;                                  \
  bool found_this_object = false;                          \
  if (absl::IsNotFound(find_status)) {                     \
    if (!find_any_object_type) {                           \
      return find_status;                                  \
    } else {                                               \
      /* Do nothing and continue to other object types. */ \
    }                                                      \
  } else if (!find_status.ok()) {                          \
    return find_status;                                    \
  } else {                                                 \
    if (found_any_object) {                                \
      output << std::endl;                                 \
    }                                                      \
    found_any_object = true;                               \
    found_this_object = true;                              \
  }                                                        \
  if (found_this_object)

  if (find_any_object_type || object_type == "table") {
    const Table* table;
    IF_FOUND(catalog->FindTable(name_path, &table)) {
      output << "Table: " << table->FullName()
             << (table->IsValueTable() ? " (value table)" : "") << std::endl;
      int first_column = 0;
      if (table->IsValueTable()) {
        ZETASQL_RET_CHECK_GE(table->NumColumns(), 1);
        output << "Row type: "
               << table->GetColumn(0)->GetType()->ShortTypeName(product_mode)
               << std::endl;
        ++first_column;
      }
      if (table->NumColumns() > first_column) {
        output << "Columns:" << std::endl;
        size_t max_column_length = 0;
        for (int idx = first_column; idx < table->NumColumns(); ++idx) {
          max_column_length =
              std::max(max_column_length, table->GetColumn(idx)->Name().size());
        }
        for (int idx = first_column; idx < table->NumColumns(); ++idx) {
          const Column* column = table->GetColumn(idx);
          output << absl::StrFormat("  %-*s ", max_column_length,
                                    column->Name())
                 << column->GetType()->ShortTypeName(product_mode)
                 << (column->IsPseudoColumn() ? " (pseudo-column)" : "")
                 << std::endl;
        }
      }
      // This could include more of the other metadata that's available on
      // Table or Column.  It currently just has the main schema details.
    }
  }

  if (find_any_object_type || object_type == "function") {
    const Function* function;
    IF_FOUND(catalog->FindFunction(name_path, &function)) {
      ZETASQL_RET_CHECK(function != nullptr);

      // This function generates signatures in one string split by "; ".
      // There isn't a method that returns them in a vector.
      // We split it because we want to print them on separate lines.
      int num_signatures;
      const std::string signatures =
          function->GetSupportedSignaturesUserFacingText(
              config.analyzer_options().language(),
              FunctionArgumentType::NamePrintingStyle::kIfNotPositionalOnly,
              &num_signatures,
              /*print_template_details=*/true);
      const std::vector<absl::string_view> split_signatures =
          absl::StrSplit(signatures, "; ");

      // This generates something like:
      //   Function XYZ
      //   Signature: XYZ(STRING) -> STRING
      //   Signature: XYZ(INT64) -> INT64
      // There are many ways to improve this but code for formatting signatures
      // is scattered and complex.
      output << function->QualifiedSQLName(/*capitalize_qualifier=*/true)
             << std::endl;
      ZETASQL_RET_CHECK_EQ(split_signatures.size(), function->NumSignatures());
      for (int i = 0; i < split_signatures.size(); ++i) {
        // The method above doesn't include the return type so we add it.
        output << "Signature: " << split_signatures[i] << " -> "
               << function->GetSignature(i)->result_type().UserFacingName(
                      product_mode, /*print_template_details=*/true)
               << std::endl;
      }
    }
  }

  if (find_any_object_type || object_type == "tvf") {
    const TableValuedFunction* tvf;
    IF_FOUND(catalog->FindTableValuedFunction(name_path, &tvf)) {
      ZETASQL_RET_CHECK(tvf != nullptr);
      ZETASQL_RET_CHECK_EQ(tvf->NumSignatures(), 1);
      const FunctionArgumentType& result_type =
          tvf->GetSignature(0)->result_type();

      output << "Table-valued function: "
             << tvf->GetSupportedSignaturesUserFacingText(
                    config.analyzer_options().language(),
                    /*print_template_and_name_details=*/true)
             << " -> "
             // Result types with and without table schemas are available
             // from different methods.
             << (result_type.options().has_relation_input_schema()
                     ? result_type.options()
                           .relation_input_schema()
                           .GetSQLDeclaration(product_mode)
                     : result_type.UserFacingName(
                           product_mode,
                           /*print_template_details=*/true))
             << std::endl;
    }
  }

  if (!called_any_find) {
    return zetasql_base::InvalidArgumentErrorBuilder()
           << "Unsupported object type " << object_type;
  }
  if (!found_any_object) {
    return zetasql_base::InvalidArgumentErrorBuilder() << "Object not found";
  }

  ZETASQL_RETURN_IF_ERROR(writer.executed(output.str()));

  return absl::OkStatus();
}

// Recursively collects all the catalogs that are enumerable.
absl::Status GetEnumerableCatalogs(
    const Catalog* catalog,
    absl::flat_hash_set<const EnumerableCatalog*>& enumerable_catalogs) {
  if (!catalog->Is<EnumerableCatalog>()) {
    return absl::OkStatus();
  }

  const EnumerableCatalog* enumerable_catalog =
      catalog->GetAs<EnumerableCatalog>();
  auto result = enumerable_catalogs.insert(enumerable_catalog);
  if (!result.second) {
    return absl::OkStatus();
  }

  if (result.second) {
    absl::flat_hash_set<const Catalog*> nested_catalogs;
    ZETASQL_RETURN_IF_ERROR(enumerable_catalog->GetCatalogs(&nested_catalogs));
    for (const auto& nested : nested_catalogs) {
      ZETASQL_RETURN_IF_ERROR(GetEnumerableCatalogs(nested, enumerable_catalogs));
    }
  }
  return absl::OkStatus();
}

// This is a rudimentary implementation of
//   SHOW object_type name [LIKE pattern]
// It can print a simple list of tables, functions and TVFs.
// If object_type is one of those, it'll print objects just of that type.
//
// This is implemented in execute_query rather than in the reference
// implementation because SHOW is engine-defined behavior and does not
// have a specified correct answer that could be compliance tested.
//
// Note that [object_type] is in plural, i.e. SHOW TABLES rather than SHOW TABLE
static absl::Status ExecuteShow(const ResolvedNode* resolved_node,
                                ExecuteQueryConfig& config,
                                ExecuteQueryWriter& writer) {
  ZETASQL_RET_CHECK_EQ(resolved_node->node_kind(), RESOLVED_SHOW_STMT);
  const ResolvedShowStmt* show_stmt = resolved_node->GetAs<ResolvedShowStmt>();
  const std::string object_type =
      absl::AsciiStrToLower(show_stmt->identifier());

  // Set up a filter for the LIKE pattern.
  std::unique_ptr<RE2> like_regexp;
  if (show_stmt->like_expr() != nullptr) {
    Value like_pattern = show_stmt->like_expr()->value();
    ZETASQL_RET_CHECK(like_pattern.has_content());
    if (like_pattern.type_kind() != TypeKind::TYPE_STRING) {
      return zetasql_base::InvalidArgumentErrorBuilder()
             << "The LIKE pattern must be a string.";
    }

    RE2::Options options;
    options.set_case_sensitive(false);
    ZETASQL_RETURN_IF_ERROR(functions::CreateLikeRegexpWithOptions(
        like_pattern.string_value(), options, &like_regexp));
  }
  absl::AnyInvocable<bool(const std::string&)> like_filter =
      [&like_regexp](absl::string_view value) -> bool {
    return like_regexp ? RE2::FullMatch(value, *like_regexp) : true;
  };

  // We can't look for objects in config.catalog() directly, since that is a
  // MultiCatalog which is not enumerable. Instead, we'll collect all the
  // enumerable catalogs accessible from the main catalog and use that set.
  absl::flat_hash_set<const EnumerableCatalog*> enumerable_catalogs;
  const Catalog* catalog = config.catalog();
  ZETASQL_RET_CHECK(catalog->Is<MultiCatalog>()) << "Expected a MultiCatalog";
  if (catalog->Is<MultiCatalog>()) {
    const MultiCatalog* multi_catalog = catalog->GetAs<MultiCatalog>();
    for (const auto& child_catalog : multi_catalog->catalogs()) {
      ZETASQL_RETURN_IF_ERROR(
          GetEnumerableCatalogs(child_catalog, enumerable_catalogs));
    }
  }

  auto sorted_strings = [](const absl::flat_hash_set<std::string>& set) {
    std::vector<std::string> v(set.begin(), set.end());
    std::sort(v.begin(), v.end(), zetasql_base::CaseLess());
    return absl::StrJoin(v, "\n");
  };

  int object_count = 0;
  if (object_type == "tables") {
    absl::flat_hash_set<std::string> table_names;
    for (const EnumerableCatalog* enumerable_catalog : enumerable_catalogs) {
      absl::flat_hash_set<const Table*> tables;
      ZETASQL_RETURN_IF_ERROR(enumerable_catalog->GetTables(&tables));
      for (const auto& table : tables) {
        if (like_filter(table->Name())) {
          table_names.insert(table->Name());
        }
      }
    }
    object_count += table_names.size();
    ZETASQL_RETURN_IF_ERROR(writer.executed(sorted_strings(table_names)));
  } else if (object_type == "functions") {
    absl::flat_hash_set<std::string> function_names;
    for (const EnumerableCatalog* enumerable_catalog : enumerable_catalogs) {
      absl::flat_hash_set<const Function*> functions;
      ZETASQL_RETURN_IF_ERROR(enumerable_catalog->GetFunctions(&functions));
      for (const auto& function : functions) {
        if (IsInternalAlias(function->Name())) continue;
        const std::string& full_name =
            function->FullName(/*include_group=*/false);
        if (like_filter(full_name)) {
          function_names.insert(full_name);
        }
      }
    }
    object_count += function_names.size();
    ZETASQL_RETURN_IF_ERROR(writer.executed(sorted_strings(function_names)));
  } else if (object_type == "tvfs") {
    absl::flat_hash_set<std::string> tvf_names;
    for (const EnumerableCatalog* enumerable_catalog : enumerable_catalogs) {
      absl::flat_hash_set<const TableValuedFunction*> tvfs;
      ZETASQL_RETURN_IF_ERROR(enumerable_catalog->GetTableValuedFunctions(&tvfs));
      for (const auto& tvf : tvfs) {
        if (like_filter(tvf->Name())) {
          tvf_names.insert(tvf->Name());
        }
      }
    }
    object_count += tvf_names.size();
    ZETASQL_RETURN_IF_ERROR(writer.executed(sorted_strings(tvf_names)));
  } else {
    return zetasql_base::InvalidArgumentErrorBuilder()
           << "Unsupported object type: " << show_stmt->identifier();
  }

  // TODO: Remove this once we have table output.
  if (object_count == 0) {
    ZETASQL_RETURN_IF_ERROR(
        writer.executed(absl::StrCat("No matching ", object_type, " found")));
  }

  return absl::OkStatus();
}

static absl::StatusOr<absl::string_view> ExtractNextStatement(
    absl::string_view script, const ExecuteQueryConfig& config,
    const ParseResumeLocation& start_location) {
  // Use GetParseTokens to find the end of the statement.
  std::vector<ParseToken> tokens;
  ParseResumeLocation end_location = start_location;
  ZETASQL_RETURN_IF_ERROR(
      GetParseTokens({.stop_at_end_of_statement = true,
                      .language_options = config.analyzer_options().language()},
                     &end_location, &tokens));

  absl::string_view statement_text = script.substr(
      start_location.byte_position(),
      end_location.byte_position() - start_location.byte_position());

  // Strip leading and trailing newlines.  From the web input, they show up
  // with both \n and \r.
  while (!statement_text.empty() &&
         (statement_text.front() == '\n' || statement_text.front() == '\r')) {
    statement_text.remove_prefix(1);
  }
  while (!statement_text.empty() &&
         (statement_text.back() == '\n' || statement_text.back() == '\r')) {
    statement_text.remove_suffix(1);
  }

  return statement_text;
}

static absl::Status ExecuteScript(absl::string_view script,
                                  ExecuteQueryConfig& config,
                                  ExecuteQueryWriter& writer) {
  std::unique_ptr<ParserOutput> parser_output;
  absl::StatusOr<const ASTNode*> ast =
      ParseSql(script, config, nullptr, nullptr, &parser_output);
  if (!ast.ok()) {
    return MaybeUpdateErrorFromPayload(
        ErrorMessageOptions{
            .mode = ErrorMessageMode::ERROR_MESSAGE_MULTI_LINE_WITH_CARET,
            .attach_error_location_payload = false},
        script, ast.status());
  }

  if (config.has_tool_mode(ToolMode::kParse) ||
      config.has_tool_mode(ToolMode::kUnparse)) {
    ZETASQL_RETURN_IF_ERROR(WriteParsedAndOrUnparsedAst(*ast, config, writer));
  }
  if (config.has_tool_mode(ToolMode::kExecute)) {
    ZETASQL_RETURN_IF_ERROR(ExplainAndOrExecuteSql(nullptr, config, writer, script));
  }
  if (!config.has_tool_mode(ToolMode::kExecute) &&
      (config.has_tool_mode(ToolMode::kResolve) ||
       config.has_tool_mode(ToolMode::kUnAnalyze))) {
    return absl::UnavailableError(
        "Analysis is not supported for scripts. "
        "Individual statements can be analyzed while they are executed if "
        "both \"execute\" and \"analyze\" are specified.");
  }
  if (config.has_tool_mode(ToolMode::kExplain)) {
    return absl::UnavailableError("Explanation is not supported for scripts.");
  }
  return absl::OkStatus();
}

// Process the next statement from script, updating `parse_result_location`
// and `at_end_of_input` on success.
static absl::Status ExecuteOneQuery(absl::string_view script,
                                    ExecuteQueryConfig& config,
                                    ExecuteQueryWriter& writer,
                                    ParseResumeLocation* parse_resume_location,
                                    bool* at_end_of_input) {
  ZETASQL_ASSIGN_OR_RETURN(
      absl::string_view statement_text,
      ExtractNextStatement(script, config, *parse_resume_location));
  ZETASQL_RETURN_IF_ERROR(writer.statement_text(statement_text));

  std::unique_ptr<ParserOutput> parser_output;
  absl::StatusOr<const ASTNode*> ast = ParseSql(
      script, config, parse_resume_location, at_end_of_input, &parser_output);
  if (!ast.ok()) {
    return MaybeUpdateErrorFromPayload(
        ErrorMessageOptions{
            .mode = ErrorMessageMode::ERROR_MESSAGE_MULTI_LINE_WITH_CARET,
            .attach_error_location_payload = false},
        script, ast.status());
  }

  if (config.has_tool_mode(ToolMode::kParse) ||
      config.has_tool_mode(ToolMode::kUnparse)) {
    ZETASQL_RETURN_IF_ERROR(WriteParsedAndOrUnparsedAst(*ast, config, writer));
  }

  // Macro support is enabled by intercepting `DEFINE MACRO` statements in the
  // parsing phase to update a persistent macro catalog, that is supplied as a
  // ParserOption and reused by subsequent calls to this method.
  //
  // TODO: Macro registration should ideally be performed in the
  // analysis phase by intercepting ResolvedDefineMacroStmt nodes instead, but
  // we don't have analyzer support yet.
  if (kMacroExpansionMode != parser::MacroExpansionMode::kNone) {
    ZETASQL_ASSIGN_OR_RETURN(bool macro_registered,
                     RegisterMacro(statement_text, config, writer));
    if (macro_registered) {
      return absl::OkStatus();
    }
  }

  if (!IsAnalysisRequired(config)) {
    return absl::OkStatus();
  }

  std::unique_ptr<const AnalyzerOutput> analyzer_output;
  ZETASQL_ASSIGN_OR_RETURN(const ResolvedNode* resolved_node,
                   AnalyzeSql(script, *ast, config, writer, &analyzer_output));
  ZETASQL_RET_CHECK_NE(resolved_node, nullptr);

  const ExecuteQueryConfig::ExamineResolvedASTCallback callback =
      config.examine_resolved_ast_callback();
  if (callback) {
    ZETASQL_RETURN_IF_ERROR(callback(resolved_node));
  }

  // This transfers ownership of `parser_output` and `analyzer_output` and
  // clears the copies here if this creates any DDL objects.
  bool executed_as_ddl = false;
  ZETASQL_RETURN_IF_ERROR(HandleDDL(resolved_node, config, writer, &parser_output,
                            &analyzer_output, &executed_as_ddl));

  if (config.has_tool_mode(ToolMode::kUnAnalyze)) {
    ZETASQL_RETURN_IF_ERROR(UnanalyzeQuery(resolved_node, config, writer));
  }

  if (config.has_tool_mode(ToolMode::kExplain) ||
      config.has_tool_mode(ToolMode::kExecute)) {
    if (resolved_node->node_kind() == RESOLVED_QUERY_STMT ||
        resolved_node->IsExpression()) {
      ZETASQL_RETURN_IF_ERROR(
          ExplainAndOrExecuteSql(resolved_node, config, writer, script));
    } else if (config.has_tool_mode(ToolMode::kExplain)) {
      return absl::InvalidArgumentError(
          absl::StrCat("The statement ", resolved_node->node_kind_string(),
                       " is not supported for explanation."));
    } else {
      ZETASQL_RET_CHECK(config.has_tool_mode(ToolMode::kExecute));
      if (resolved_node->node_kind() == RESOLVED_DESCRIBE_STMT) {
        ZETASQL_RETURN_IF_ERROR(ExecuteDescribe(resolved_node, config, writer));
      } else if (resolved_node->node_kind() == RESOLVED_SHOW_STMT) {
        ZETASQL_RETURN_IF_ERROR(ExecuteShow(resolved_node, config, writer));
      } else if (executed_as_ddl) {
        // Execution handled in HandleDDL above.
      } else {
        return absl::InvalidArgumentError(
            absl::StrCat("The statement ", resolved_node->node_kind_string(),
                         " is not supported for execution."));
      }
    }
  }

  return absl::OkStatus();
}

absl::Status ExecuteQuery(absl::string_view sql, ExecuteQueryConfig& config,
                          ExecuteQueryWriter& writer) {
  std::string script(sql);
  if (config.sql_mode() == SqlMode::kScript) {
    ZETASQL_RETURN_IF_ERROR(ExecuteScript(script, config, writer));
    return absl::OkStatus();
  }

  auto parse_resume_location = ParseResumeLocation::FromString(script);
  bool at_end_of_input = false;

  bool is_first = true;
  while (!at_end_of_input) {
    ZETASQL_RETURN_IF_ERROR(writer.StartStatement(is_first));
    is_first = false;

    // This currently stops execution on the first statement with an error.
    // Recovering after finding the next parse_resume_location requires some
    // new code.
    ZETASQL_RETURN_IF_ERROR(ExecuteOneQuery(script, config, writer,
                                    &parse_resume_location, &at_end_of_input));
  }

  return absl::OkStatus();
}

}  // namespace zetasql
