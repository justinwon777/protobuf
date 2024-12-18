// Protocol Buffers - Google's data interchange format
// Copyright 2023 Google LLC.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "google/protobuf/compiler/rust/generator.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/types/span.h"
#include "google/protobuf/compiler/code_generator.h"
#include "google/protobuf/compiler/cpp/names.h"
#include "google/protobuf/compiler/rust/context.h"
#include "google/protobuf/compiler/rust/crate_mapping.h"
#include "google/protobuf/compiler/rust/enum.h"
#include "google/protobuf/compiler/rust/message.h"
#include "google/protobuf/compiler/rust/naming.h"
#include "google/protobuf/compiler/rust/relative_path.h"
#include "google/protobuf/compiler/versions.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/io/printer.h"

namespace google {
namespace protobuf {
namespace compiler {
namespace rust {
namespace {

// Emits `pub use <crate_name>::<modules for parent types>::Type` for all
// messages and enums of a `dep`. This should only be
// called for 'import public' deps.
void EmitPublicImportsForDepFile(Context& ctx, const FileDescriptor* dep) {
  std::string crate_name = GetCrateName(ctx, *dep);
  for (int i = 0; i < dep->message_type_count(); ++i) {
    auto* msg = dep->message_type(i);
    auto path = RsTypePath(ctx, *msg);
    ctx.Emit({{"pkg::Msg", path}},
             R"rs(
                pub use $pkg::Msg$;
                pub use $pkg::Msg$View;
                pub use $pkg::Msg$Mut;
              )rs");
  }
  for (int i = 0; i < dep->enum_type_count(); ++i) {
    auto* enum_ = dep->enum_type(i);
    auto path = RsTypePath(ctx, *enum_);
    ctx.Emit({{"pkg::Enum", path}},
             R"rs(
                pub use $pkg::Enum$;
              )rs");
  }
}

// Emits public imports of all files coming from dependencies (imports of local
// files are implicitly public).
//
// `import public` works transitively in C++ (although it doesn't respect
// layering_check in clang). For Rust we actually make it layering clean because
// Blaze compiles transitive proto deps as if they were direct.
//
// Note we don't reexport entire crates, only messages and enums from files that
// have been explicitly publicly imported. It may happen that a `proto_library`
// defines multiple files, but not all are publicly imported.
void EmitPublicImports(const RustGeneratorContext& rust_generator_context,
                       Context& ctx, const FileDescriptor& file) {
  std::vector<const FileDescriptor*> files_to_visit{&file};
  while (!files_to_visit.empty()) {
    const FileDescriptor* f = files_to_visit.back();
    files_to_visit.pop_back();

    if (!rust_generator_context.is_file_in_current_crate(*f)) {
      EmitPublicImportsForDepFile(ctx, f);
    }

    for (int i = 0; i < f->public_dependency_count(); ++i) {
      files_to_visit.push_back(f->public_dependency(i));
    }
  }
}

void EmitEntryPointRsFile(GeneratorContext* generator_context,
                          Context& ctx_without_printer,
                          const std::vector<const FileDescriptor*>& files) {
  // Besides the one .rs file per .proto file, we additional emit one
  // entry_point rs file here which re-exports all of the types generated by
  // this same proto_library.
  std::string entry_point_rs_file_path =
      GetEntryPointRsFilePath(ctx_without_printer, *files.front());
  auto outfile =
      absl::WrapUnique(generator_context->Open(entry_point_rs_file_path));
  io::Printer printer(outfile.get());
  Context ctx = ctx_without_printer.WithPrinter(&printer);

  // Declare the submodules for all of the the generated code and pub re-export
  // all of them into a flat namespace.
  RelativePath primary_relpath(entry_point_rs_file_path);
  for (const FileDescriptor* file : files) {
    std::string non_primary_file_path = GetRsFile(ctx, *file);
    std::string relative_mod_path =
        primary_relpath.Relative(RelativePath(non_primary_file_path));
    // Temporarily emit these re-exported mods as pub to avoid issues with
    // Crubit. In a future change we should change these back to be private
    // mods.
    ctx.Emit({{"file_path", relative_mod_path},
              {"mod_name", RustInternalModuleName(*file)}},
             R"rs(
              #[path="$file_path$"]
              #[allow(non_snake_case)]
              pub mod internal_do_not_use_$mod_name$;

              #[allow(unused_imports)]
              pub use internal_do_not_use_$mod_name$::*;
            )rs");
  }
}

}  // namespace

bool RustGenerator::Generate(const FileDescriptor* file,
                             const std::string& parameter,
                             GeneratorContext* generator_context,
                             std::string* error) const {
  absl::StatusOr<Options> opts = Options::Parse(parameter);
  if (!opts.ok()) {
    *error = std::string(opts.status().message());
    return false;
  }

  std::vector<const FileDescriptor*> files_in_current_crate;
  generator_context->ListParsedFiles(&files_in_current_crate);

  absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
      import_path_to_crate_name = GetImportPathToCrateNameMap(&*opts);
  if (!import_path_to_crate_name.ok()) {
    *error = std::string(import_path_to_crate_name.status().message());
    return false;
  }

  RustGeneratorContext rust_generator_context(&files_in_current_crate,
                                              &*import_path_to_crate_name);

  std::vector<std::string> modules;
  modules.emplace_back(RustInternalModuleName(*file));
  Context ctx_without_printer(&*opts, &rust_generator_context, nullptr,
                              std::move(modules));

  auto outfile = absl::WrapUnique(
      generator_context->Open(GetRsFile(ctx_without_printer, *file)));
  io::Printer printer(outfile.get());
  Context ctx = ctx_without_printer.WithPrinter(&printer);

  // Convenience shorthands for common symbols.
  auto v = ctx.printer().WithVars({
      {"std", "::std"},
      {"pb", "::protobuf"},
      {"pbi", "::protobuf::__internal"},
      {"pbr", "::protobuf::__internal::runtime"},
      {"NonNull", "::std::ptr::NonNull"},
      {"Phantom", "::std::marker::PhantomData"},
      {"Result", "::std::result::Result"},
      {"Option", "::std::option::Option"},
  });

  std::string expected_runtime_version = absl::StrCat(
      absl::StripSuffix(PROTOBUF_RUST_VERSION_STRING, "-dev"), "-beta");

  ctx.Emit({{"expected_runtime_version", expected_runtime_version}},
           R"rs(
    const _: () = $pbi$::assert_compatible_gencode_version("$expected_runtime_version$");
  )rs");

  std::vector<const FileDescriptor*> file_contexts(
      files_in_current_crate.begin(), files_in_current_crate.end());

  // When the generator is called for the 'first' file we also want to emit the
  // 'entry point' rs file. This is the file that will simply pub re-export all
  // everything from all of the other generated .rs files.
  if (file == files_in_current_crate.front()) {
    EmitEntryPointRsFile(generator_context, ctx_without_printer, file_contexts);
  }

  std::unique_ptr<io::ZeroCopyOutputStream> thunks_cc;
  std::unique_ptr<io::Printer> thunks_printer;
  if (ctx.is_cpp()) {
    thunks_cc.reset(generator_context->Open(GetThunkCcFile(ctx, *file)));
    thunks_printer = std::make_unique<io::Printer>(thunks_cc.get());

    thunks_printer->Emit(
        {{"proto_h", GetHeaderFile(ctx, *file)},
         {"proto_deps_h",
          [&] {
            for (int i = 0; i < file->dependency_count(); i++) {
              if (opts->strip_nonfunctional_codegen &&
                  IsKnownFeatureProto(file->dependency(i)->name())) {
                // Strip feature imports for editions codegen tests.
                continue;
              }
              thunks_printer->Emit(
                  {{"proto_dep_h", GetHeaderFile(ctx, *file->dependency(i))}},
                  R"cc(
#include "$proto_dep_h$"
                  )cc");
            }
          }}},
        R"cc(
#include "$proto_h$"
          $proto_deps_h$
#include "google/protobuf/map.h"
#include "google/protobuf/repeated_field.h"
#include "google/protobuf/repeated_ptr_field.h"
#include "rust/cpp_kernel/serialized_data.h"
#include "rust/cpp_kernel/strings.h"
        )cc");
  }

  EmitPublicImports(rust_generator_context, ctx, *file);

  for (int i = 0; i < file->message_type_count(); ++i) {
    auto& msg = *file->message_type(i);

    GenerateRs(ctx, msg);
    ctx.printer().PrintRaw("\n");

    if (ctx.is_cpp()) {
      auto thunks_ctx = ctx.WithPrinter(thunks_printer.get());

      thunks_ctx.Emit({{"Msg", msg.full_name()}}, R"cc(
        // $Msg$
      )cc");
      GenerateThunksCc(thunks_ctx, msg);
      thunks_ctx.printer().PrintRaw("\n");
    }
  }

  for (int i = 0; i < file->enum_type_count(); ++i) {
    auto& enum_ = *file->enum_type(i);
    GenerateEnumDefinition(ctx, enum_);
    ctx.printer().PrintRaw("\n");

    if (ctx.is_cpp()) {
      auto thunks_ctx = ctx.WithPrinter(thunks_printer.get());

      thunks_ctx.Emit({{"enum", enum_.full_name()}}, R"cc(
        // $enum$
      )cc");
      thunks_ctx.printer().PrintRaw("\n");
    }
  }

  return true;
}

}  // namespace rust
}  // namespace compiler
}  // namespace protobuf
}  // namespace google
