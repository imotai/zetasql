#
# Copyright 2019 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

load("@bazel_skylib//:bzl_library.bzl", "bzl_library")

package(
    default_visibility = ["//zetasql/base:zetasql_implementation"],
)

cc_library(
    name = "control_flow_graph",
    srcs = ["control_flow_graph.cc"],
    hdrs = ["control_flow_graph.h"],
    deps = [
        ":script_segment",
        "//zetasql/base:map_util",
        "//zetasql/base:status",
        "//zetasql/parser",
        "//zetasql/public:parse_location",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "parsed_script",
    srcs = ["parsed_script.cc"],
    hdrs = [
        "parsed_script.h",
    ],
    deps = [
        ":control_flow_graph",
        ":error_helpers",
        ":type_aliases",
        "//zetasql/base:check",
        "//zetasql/base:map_util",
        "//zetasql/base:status",
        "//zetasql/base:strings",
        "//zetasql/common:errors",
        "//zetasql/parser",
        "//zetasql/public:error_helpers",
        "//zetasql/public:id_string",
        "//zetasql/public:options_cc_proto",
        "//zetasql/public:parse_location",
        "//zetasql/public:type",
        "@com_google_absl//absl/algorithm:container",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:btree",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:variant",
    ],
)

cc_test(
    name = "parsed_script_test",
    srcs = ["parsed_script_test.cc"],
    deps = [
        ":parsed_script",
        ":type_aliases",
        "//zetasql/base:source_location",
        "//zetasql/base/testing:status_matchers",
        "//zetasql/base/testing:zetasql_gtest_main",
        "//zetasql/parser",
        "//zetasql/public:id_string",
        "//zetasql/public:parse_location",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
        "@com_google_absl//absl/types:variant",
    ],
)

cc_library(
    name = "script_segment",
    srcs = ["script_segment.cc"],
    hdrs = ["script_segment.h"],
    deps = [
        "//zetasql/parser",
        "//zetasql/public:parse_location",
        "//zetasql/public:parse_resume_location",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "parse_helpers",
    srcs = ["parse_helpers.cc"],
    hdrs = ["parse_helpers.h"],
    deps = [
        ":parsed_script",
        "//zetasql/base:status",
        "//zetasql/common:errors",
        "//zetasql/parser",
        "//zetasql/public:error_helpers",
        "//zetasql/public:options_cc_proto",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "error_helpers",
    srcs = ["error_helpers.cc"],
    hdrs = ["error_helpers.h"],
    deps = [
        ":script_segment",
        "//zetasql/base:status",
        "//zetasql/common:errors",
        "//zetasql/common:status_payload_utils",
        "//zetasql/parser",
        "//zetasql/proto:script_exception_cc_proto",
        "//zetasql/public:error_location_cc_proto",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "stack_frame",
    hdrs = ["stack_frame.h"],
    deps = [
        ":parsed_script",
        "//zetasql/parser",
    ],
)

cc_library(
    name = "type_aliases",
    hdrs = ["type_aliases.h"],
    deps = [
        "//zetasql/public:id_string",
        "//zetasql/public:value",
    ],
)

cc_library(
    name = "script_executor",
    srcs = [
        "script_executor.cc",
        "script_executor_impl.cc",
        "script_executor_impl.h",
        "serialization_helpers.cc",
        "serialization_helpers.h",
    ],
    hdrs = [
        "script_executor.h",
    ],
    deps = [
        ":control_flow_graph",
        ":error_helpers",
        ":parsed_script",
        ":procedure_extension_cc_proto",
        ":script_executor_state_cc_proto",
        ":script_segment",
        ":stack_frame",
        ":type_aliases",
        ":variable_cc_proto",
        "//zetasql/base",
        "//zetasql/base:flat_set",
        "//zetasql/base:map_util",
        "//zetasql/base:ret_check",
        "//zetasql/base:status",
        "//zetasql/common:errors",
        "//zetasql/common:status_payload_utils",
        "//zetasql/parser",
        "//zetasql/proto:script_exception_cc_proto",
        "//zetasql/public:analyzer",
        "//zetasql/public:analyzer_options",
        "//zetasql/public:coercer",
        "//zetasql/public:error_helpers",
        "//zetasql/public:evaluator",
        "//zetasql/public:evaluator_table_iterator",
        "//zetasql/public:function",
        "//zetasql/public:id_string",
        "//zetasql/public:language_options",
        "//zetasql/public:options_cc_proto",
        "//zetasql/public:parse_location",
        "//zetasql/public:signature_match_result",
        "//zetasql/public:time_zone_util",
        "//zetasql/public:type",
        "//zetasql/public/types",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:span",
    ],
)

cc_test(
    name = "error_helpers_test",
    srcs = ["error_helpers_test.cc"],
    deps = [
        ":error_helpers",
        ":script_segment",
        "//zetasql/base:status",
        "//zetasql/base/testing:status_matchers",
        "//zetasql/base/testing:zetasql_gtest_main",
        "//zetasql/common:errors",
        "//zetasql/common:status_payload_utils",
        "//zetasql/parser",
        "//zetasql/proto:internal_error_location_cc_proto",
        "//zetasql/public:error_helpers",
        "//zetasql/public:options_cc_proto",
        "//zetasql/public:parse_location",
    ],
)

proto_library(
    name = "script_executor_state_proto",
    srcs = ["script_executor_state.proto"],
    deps = [
        ":procedure_extension_proto",
        ":variable_proto",
        "//zetasql/proto:function_proto",
        "//zetasql/proto:script_exception_proto",
        "@com_google_protobuf//:any_proto",
    ],
)

cc_proto_library(
    name = "script_executor_state_cc_proto",
    deps = [":script_executor_state_proto"],
)

proto_library(
    name = "procedure_extension_proto",
    srcs = ["procedure_extension.proto"],
)

cc_proto_library(
    name = "procedure_extension_cc_proto",
    deps = [":procedure_extension_proto"],
)

proto_library(
    name = "variable_proto",
    srcs = ["variable.proto"],
    deps = [
        "//zetasql/public:type_parameters_proto",
        "//zetasql/public:type_proto",
        "//zetasql/public:value_proto",
    ],
)

cc_proto_library(
    name = "variable_cc_proto",
    deps = [":variable_proto"],
)

java_proto_library(
    name = "variable_java_proto",
    deps = [":variable_proto"],
)
