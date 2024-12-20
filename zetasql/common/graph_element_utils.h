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

#ifndef ZETASQL_COMMON_GRAPH_ELEMENT_UTILS_H_
#define ZETASQL_COMMON_GRAPH_ELEMENT_UTILS_H_

#include "zetasql/public/types/type.h"
namespace zetasql {

class Function;

// Helper function that is used to determine a type is or contains in its
// nesting structure a GraphElement or GraphPath type.
bool TypeIsOrContainsGraphElement(const Type* type);

}  // namespace zetasql

#endif  // ZETASQL_COMMON_GRAPH_ELEMENT_UTILS_H_
