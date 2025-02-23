#include <sourcemeta/core/alterschema.h>
#include <sourcemeta/core/json.h>
#include <sourcemeta/core/jsonpointer.h>
#include <sourcemeta/core/jsonschema.h>

#include <cstdlib>  // EXIT_SUCCESS, EXIT_FAILURE
#include <fstream>  // std::ofstream
#include <iostream> // std::cerr, std::cout
#include <sstream>  // std::ostringstream

#include "command.h"
#include "utils.h"

auto sourcemeta::jsonschema::cli::lint(
    const std::span<const std::string> &arguments) -> int {
  const auto options{parse_options(arguments, {"f", "fix", "json", "j"})};
  const bool fix = options.contains("f") || options.contains("fix");
  const bool output_json = options.contains("json") || options.contains("j");

  sourcemeta::core::SchemaTransformer bundle;
  sourcemeta::core::add(bundle,
                        sourcemeta::core::AlterSchemaCategory::AntiPattern);
  sourcemeta::core::add(bundle,
                        sourcemeta::core::AlterSchemaCategory::Simplify);
  sourcemeta::core::add(bundle,
                        sourcemeta::core::AlterSchemaCategory::Superfluous);
  sourcemeta::core::add(bundle,
                        sourcemeta::core::AlterSchemaCategory::Redundant);
  sourcemeta::core::add(bundle,
                        sourcemeta::core::AlterSchemaCategory::SyntaxSugar);

  bool result{true};
  auto errors_array = sourcemeta::core::JSON::make_array();

  if (options.contains("f") || options.contains("fix")) {
    for (const auto &entry :
         for_each_json(options.at(""), parse_ignore(options),
                       parse_extensions(options))) {
      log_verbose(options) << "Linting: " << entry.first.string() << "\n";
      if (entry.first.extension() == ".yaml" ||
          entry.first.extension() == ".yml") {
        std::cerr << "The --fix option is not supported for YAML input files\n";
        return EXIT_FAILURE;
      }

      auto copy = entry.second;
      bundle.apply(copy, sourcemeta::core::schema_official_walker,
                   resolver(options));
      std::ofstream output{entry.first};
      sourcemeta::core::prettify(copy, output,
                                 sourcemeta::core::schema_format_compare);
      output << "\n";
    }
  }
  for (const auto &entry : for_each_json(options.at(""), parse_ignore(options),
                                         parse_extensions(options))) {
    log_verbose(options) << "Linting: " << entry.first.string() << "\n";
    const bool subresult = bundle.check(
        entry.second, sourcemeta::core::schema_official_walker,
        resolver(options),
        [&](const auto &pointer, const auto &name, const auto &message) {
          if (output_json) {
            auto error_obj = sourcemeta::core::JSON::make_object();

            error_obj.assign("path",
                             sourcemeta::core::JSON{entry.first.string()});
            error_obj.assign("id", sourcemeta::core::JSON{name});
            error_obj.assign("message", sourcemeta::core::JSON{message});

            std::ostringstream pointer_stream;
            sourcemeta::core::stringify(pointer, pointer_stream);
            error_obj.assign("schemaLocation",
                             sourcemeta::core::JSON{pointer_stream.str()});

            errors_array.push_back(error_obj);
          } else {
            std::cout << entry.first.string() << ":\n";
            std::cout << "  " << message << " (" << name << ")\n";
            std::cout << "    at schema location \"";
            sourcemeta::core::stringify(pointer, std::cout);
            std::cout << "\"\n";
          }
        });

    if (!subresult) {
      result = false;
    }
  }
  if (output_json) {
    auto output_json_object = sourcemeta::core::JSON::make_object();
    output_json_object.assign("valid", sourcemeta::core::JSON{result});
    output_json_object.assign("errors", sourcemeta::core::JSON{errors_array});
    if (fix) {
      output_json_object.assign("fixApplied", sourcemeta::core::JSON{true});
    }
    sourcemeta::core::prettify(output_json_object, std::cout);
    std::cout << "\n";
  } else {
    // If fix == true and --json was NOT specified, produce no output.
  }

  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
