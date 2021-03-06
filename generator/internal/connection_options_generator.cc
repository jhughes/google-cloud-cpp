// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "generator/internal/connection_options_generator.h"
#include "generator/internal/codegen_utils.h"
#include "generator/internal/printer.h"
#include <google/protobuf/descriptor.h>

namespace google {
namespace cloud {
namespace generator_internal {

ConnectionOptionsGenerator::ConnectionOptionsGenerator(
    google::protobuf::ServiceDescriptor const* service_descriptor,
    VarsDictionary service_vars,
    std::map<std::string, VarsDictionary> service_method_vars,
    google::protobuf::compiler::GeneratorContext* context)
    : ServiceCodeGenerator("connection_options_header_path",
                           "connection_options_cc_path", service_descriptor,
                           std::move(service_vars),
                           std::move(service_method_vars), context) {}

Status ConnectionOptionsGenerator::GenerateHeader() {
  HeaderPrint(CopyrightLicenseFileHeader());
  HeaderPrint(  // clang-format off
    "// Generated by the Codegen C++ plugin.\n"
    "// If you make any local changes, they will be lost.\n"
    "// source: $proto_file_name$\n"
    "#ifndef $header_include_guard$\n"
    "#define $header_include_guard$\n"
    "\n");
  // clang-format on

  // includes
  HeaderLocalIncludes(
      {"google/cloud/connection_options.h", "google/cloud/version.h"});
  HeaderSystemIncludes({"string"});
  HeaderPrint("\n");

  auto result = HeaderOpenNamespaces();
  if (!result.ok()) return result;

  // connection options
  HeaderPrint(  // clang-format off
    "struct ConnectionOptionsTraits {\n"
    "  static std::string default_endpoint();\n"
    "  static std::string user_agent_prefix();\n"
    "  static int default_num_channels();\n"
    "};\n\n");
  // clang-format on

  HeaderPrint(  // clang-format off
    "using ConnectionOptions =\n"
    "  google::cloud::ConnectionOptions<ConnectionOptionsTraits>;\n\n");
  // clang-format on

  HeaderCloseNamespaces();
  // close header guard
  HeaderPrint(  // clang-format off
    "#endif  // $header_include_guard$\n");
  // clang-format on
  return {};
}

Status ConnectionOptionsGenerator::GenerateCc() {
  CcPrint(CopyrightLicenseFileHeader());
  CcPrint(  // clang-format off
    "// Generated by the Codegen C++ plugin.\n"
    "// If you make any local changes, they will be lost.\n"
    "// source: $proto_file_name$\n\n");
  // clang-format on

  // includes
  CcLocalIncludes({vars("connection_options_header_path"),
                   "google/cloud/internal/user_agent_prefix.h"});
  CcSystemIncludes({"string"});
  CcPrint("\n");

  auto result = CcOpenNamespaces();
  if (!result.ok()) return result;

  CcPrint(  // clang-format off
    "std::string ConnectionOptionsTraits::default_endpoint() {\n"
    "  return \"$service_endpoint$\";\n"
    "}\n\n");
  // clang-format on

  CcPrint(  // clang-format off
    "std::string ConnectionOptionsTraits::user_agent_prefix() {\n"
    "  return google::cloud::internal::UserAgentPrefix();\n"
    "}\n\n"
    "int ConnectionOptionsTraits::default_num_channels() { return 4; }\n\n");
  // clang-format on

  CcCloseNamespaces();
  return {};
}

}  // namespace generator_internal
}  // namespace cloud
}  // namespace google
