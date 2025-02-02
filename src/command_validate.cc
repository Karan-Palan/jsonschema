#include <sourcemeta/core/json.h>
#include <sourcemeta/core/jsonl.h>
#include <sourcemeta/core/jsonschema.h>
#include <sourcemeta/core/jsonpointer.h>

#include <sourcemeta/blaze/compiler.h>
#include <sourcemeta/blaze/evaluator.h>

#include <chrono>   // std::chrono
#include <cstdlib>  // EXIT_SUCCESS, EXIT_FAILURE
#include <filesystem>
#include <iostream> // std::cerr
#include <optional> // std::optional
#include <set>      // std::set
#include <span>     // std::span
#include <string>   // std::string
#include <utility>  // std::ref

#include "command.h"
#include "utils.h"

// TODO: Add a flag to emit output using the standard JSON Schema output format
// TODO: Add a flag to collect annotations
// TODO: Add a flag to take a pre-compiled schema as input
auto sourcemeta::jsonschema::cli::validate(
    const std::span<const std::string> &arguments) -> int {
  const auto options{
      parse_options(arguments, {"h", "http", "b", "benchmark", "t", "trace",
                               "p", "path"})};

  if (options.at("").size() < 1) {
    std::cerr
        << "error: This command expects a path to a schema and a path to an\n"
        << "instance to validate against the schema. For example:\n\n"
        << "  jsonschema validate path/to/schema.json path/to/instance.json\n";
    return EXIT_FAILURE;
  }

  if (options.at("").size() < 2) {
    std::cerr
        << "error: In addition to the schema, you must also pass an argument\n"
        << "that represents the instance to validate against. For example:\n\n"
        << "  jsonschema validate path/to/schema.json path/to/instance.json\n";
    return EXIT_FAILURE;
  }

  const auto &schema_path{options.at("").at(0)};
  const auto custom_resolver{
      resolver(options, options.contains("h") || options.contains("http"))};

  const auto schema{sourcemeta::jsonschema::cli::read_file(schema_path)};

  // Extract optional JSON Pointer from --path/-p
  std::optional<std::string> pointer_option;
  if (options.contains("p") && !options.at("p").empty()) {
    pointer_option = options.at("p").at(0);
  } else if (options.contains("path") && !options.at("path").empty()) {
    pointer_option = options.at("path").at(0);
  }

  // Start by copying the entire schema, then override if pointer is given
  sourcemeta::core::JSON resolved_schema{schema};

  if (pointer_option.has_value()) {
    // Convert pointer string -> sourcemeta::core::Pointer
    const auto pointer = sourcemeta::core::to_pointer(pointer_option.value());
    // Attempt to get the sub-schema
    const auto *maybe_ptr = sourcemeta::core::try_get(schema, pointer);
    if (maybe_ptr == nullptr) {
      std::cerr << "error: Failed to resolve JSON Pointer '"
                << pointer_option.value() << "' in the provided schema\n  "
                << std::filesystem::canonical(schema_path).string() << "\n";
      return EXIT_FAILURE;
    }

    resolved_schema = *maybe_ptr;
  }

  // Validate that the final resolved_schema is indeed a valid JSON Schema
  if (!sourcemeta::core::is_schema(resolved_schema)) {
    std::cerr << "error: The schema (or sub-schema) you provided does not\n"
              << "represent a valid JSON Schema\n  "
              << std::filesystem::canonical(schema_path).string() << "\n";
    return EXIT_FAILURE;
  }

  const auto benchmark{options.contains("b") || options.contains("benchmark")};
  const auto trace{options.contains("t") || options.contains("trace")};
  const auto schema_template{sourcemeta::blaze::compile(
      resolved_schema, sourcemeta::core::default_schema_walker, custom_resolver,
      sourcemeta::blaze::default_schema_compiler)};
  sourcemeta::blaze::Evaluator evaluator;

  bool result{true};

  auto iterator{options.at("").cbegin()};
  std::advance(iterator, 1);
  for (; iterator != options.at("").cend(); ++iterator) {
    const std::filesystem::path instance_path{*iterator};
    if (instance_path.extension() == ".jsonl") {
      log_verbose(options)
          << "Interpreting input as JSONL: "
          << std::filesystem::weakly_canonical(instance_path).string() << "\n";
      std::size_t index{0};
      auto stream{sourcemeta::core::read_file(instance_path)};
      try {
        for (const auto &instance : sourcemeta::core::JSONL{stream}) {
          index += 1;
          std::ostringstream error;
          sourcemeta::blaze::ErrorOutput output{instance};
          sourcemeta::blaze::TraceOutput trace_output;
          bool subresult = true;
          if (benchmark) {
            const auto timestamp_start{
                std::chrono::high_resolution_clock::now()};
            subresult = evaluator.validate(schema_template, instance);
            const auto timestamp_end{std::chrono::high_resolution_clock::now()};
            const auto duration_us{
                std::chrono::duration_cast<std::chrono::microseconds>(
                    timestamp_end - timestamp_start)};
            if (subresult) {
              std::cout << "took: " << duration_us.count() << "us\n";
            } else {
              error << "error: Schema validation failure\n";
            }
          } else if (trace) {
            subresult = evaluator.validate(schema_template, instance,
                                           std::ref(trace_output));
          } else {
            subresult =
                evaluator.validate(schema_template, instance, std::ref(output));
          }

          if (trace) {
            print(trace_output, std::cout);
            result = subresult;
          } else if (subresult) {
            log_verbose(options)
                << "ok: "
                << std::filesystem::weakly_canonical(instance_path).string()
                << " (entry #" << index << ")"
                << "\n  matches "
                << std::filesystem::weakly_canonical(schema_path).string()
                << "\n";
          } else {
            std::cerr
                << "fail: "
                << std::filesystem::weakly_canonical(instance_path).string()
                << " (entry #" << index << ")\n\n";
            sourcemeta::core::prettify(instance, std::cerr);
            std::cerr << "\n\n";
            std::cerr << error.str();
            print(output, std::cerr);
            result = false;
            break;
          }
        }
      } catch (const sourcemeta::core::ParseError &error) {
        // For producing better error messages
        throw sourcemeta::core::FileParseError(instance_path, error);
      }

      if (index == 0) {
        log_verbose(options) << "warning: The JSONL file is empty\n";
      }
    } else {
      const auto instance{
          sourcemeta::jsonschema::cli::read_file(instance_path)};
      std::ostringstream error;
      sourcemeta::blaze::ErrorOutput output{instance};
      sourcemeta::blaze::TraceOutput trace_output;
      bool subresult{true};
      if (benchmark) {
        const auto timestamp_start{std::chrono::high_resolution_clock::now()};
        subresult = evaluator.validate(schema_template, instance);
        const auto timestamp_end{std::chrono::high_resolution_clock::now()};
        const auto duration_us{
            std::chrono::duration_cast<std::chrono::microseconds>(
                timestamp_end - timestamp_start)};
        if (subresult) {
          std::cout << "took: " << duration_us.count() << "us\n";
        } else {
          error << "error: Schema validation failure\n";
          result = false;
        }
      } else if (trace) {
        subresult = evaluator.validate(schema_template, instance,
                                       std::ref(trace_output));
      } else {
        subresult =
            evaluator.validate(schema_template, instance, std::ref(output));
      }

      if (trace) {
        print(trace_output, std::cout);
        result = subresult;
      } else if (subresult) {
        log_verbose(options)
            << "ok: "
            << std::filesystem::weakly_canonical(instance_path).string()
            << "\n  matches "
            << std::filesystem::weakly_canonical(schema_path).string() << "\n";
      } else {
        std::cerr << "fail: "
                  << std::filesystem::weakly_canonical(instance_path).string()
                  << "\n";
        std::cerr << error.str();
        print(output, std::cerr);
        result = false;
      }
    }
  }

  return result ? EXIT_SUCCESS
                // Report a different exit code for validation failures, to
                // distinguish them from other errors
                : 2;
}
