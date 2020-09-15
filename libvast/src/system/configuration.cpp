/******************************************************************************
 *                    _   _____   __________                                  *
 *                   | | / / _ | / __/_  __/     Visibility                   *
 *                   | |/ / __ |_\ \  / /          Across                     *
 *                   |___/_/ |_/___/ /_/       Space and Time                 *
 *                                                                            *
 * This file is part of VAST. It is subject to the license terms in the       *
 * LICENSE file found in the top-level directory of this distribution and at  *
 * http://vast.io/license. No part of VAST, including this file, may be       *
 * copied, modified, propagated, or distributed except according to the terms *
 * contained in the LICENSE file.                                             *
 ******************************************************************************/

#include "vast/system/configuration.hpp"

#include "vast/concept/convertible/to.hpp"
#include "vast/config.hpp"
#include "vast/data.hpp"
#include "vast/detail/add_message_types.hpp"
#include "vast/detail/append.hpp"
#include "vast/detail/assert.hpp"
#include "vast/detail/process.hpp"
#include "vast/detail/settings.hpp"
#include "vast/detail/string.hpp"
#include "vast/detail/system.hpp"
#include "vast/path.hpp"
#include "vast/synopsis_factory.hpp"
#include "vast/table_slice_builder_factory.hpp"
#include "vast/table_slice_factory.hpp"
#include "vast/value_index.hpp"
#include "vast/value_index_factory.hpp"

#include <caf/io/middleman.hpp>
#include <caf/message_builder.hpp>
#if VAST_USE_OPENCL
#  include <caf/opencl/manager.hpp>
#endif
#if VAST_USE_OPENSSL
#  include <caf/openssl/manager.hpp>
#endif

#include <algorithm>

namespace vast::system {

namespace {

template <class... Ts>
void initialize_factories() {
  (factory<Ts>::initialize(), ...);
}

} // namespace

configuration::configuration() {
  detail::add_message_types(*this);
  // Instead of the CAF-supplied `config_file_path`, we use our own
  // `config_paths` variable in order to support multiple configuration files.
  if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME"))
    config_paths.emplace_back(path{xdg_config_home} / "vast" / "vast.conf");
  else if (const char* home = std::getenv("HOME"))
    config_paths.emplace_back(path{home} / ".config" / "vast" / "vast.conf");
  config_paths.emplace_back(VAST_SYSCONFDIR "/vast/vast.conf");
  // Remove all non-existent config files.
  config_paths.erase(
    std::remove_if(config_paths.begin(), config_paths.end(),
                   [](auto&& p) { return !p.is_regular_file(); }),
    config_paths.end());
  // Load I/O module.
  load<caf::io::middleman>();
  // GPU acceleration.
#if VAST_USE_OPENCL
  load<caf::opencl::manager>();
#endif
  initialize_factories<synopsis, table_slice, table_slice_builder,
                       value_index>();
}

caf::error configuration::parse(int argc, char** argv) {
  VAST_ASSERT(argc > 0);
  VAST_ASSERT(argv != nullptr);
  command_line.assign(argv + 1, argv + argc);
  // Move CAF options to the end of the command line, parse them, and then
  // remove them.
  auto is_vast_opt = [](auto& x) { return !detail::starts_with(x, "--caf."); };
  auto caf_opt = std::stable_partition(command_line.begin(), command_line.end(),
                                       is_vast_opt);
  std::vector<std::string> caf_args;
  std::move(caf_opt, command_line.end(), std::back_inserter(caf_args));
  command_line.erase(caf_opt, command_line.end());
  // If the user provided a config file on the command line, we attempt to
  // parse it last.
  for (auto& arg : command_line)
    if (detail::starts_with(arg, "--config="))
      config_paths.push_back(arg.substr(9));
  // Parse and merge all configuration files.
  caf::settings merged_settings;
  for (const auto& config : config_paths) {
    if (exists(config)) {
      auto contents = load_contents(config);
      if (!contents)
        return contents.error();
      auto yaml = from_yaml(*contents);
      if (!yaml)
        return yaml.error();
      auto rec = caf::get_if<record>(&*yaml);
      if (!rec)
        return caf::make_error(ec::parse_error, "config file not a YAML map");
      auto flat_yaml = flatten(*rec);
      auto settings = to<caf::settings>(flat_yaml);
      if (!settings)
        return settings.error();
      detail::merge_settings(*settings, merged_settings);
    }
  }
  // Now this is incredibly ugly, but the config_option_set is the only place
  // the contains the valid type information that our config file needs to
  // abide to. So we can only adjust the content member after we have verified
  // that the type is valid. We have to do this by...parsing a string.
  // (Maybe CAF 0.18 makes this less clunky.)
  for (auto& option : custom_options_) {
    auto i = merged_settings.find(option.full_name());
    if (i != merged_settings.end()) {
      auto& val = i->second;
      // We have flattened the YAML contents above.
      VAST_ASSERT(!caf::holds_alternative<caf::config_value::dictionary>(val));
      // Ugly hack to obtain a string representation without quotes.
      auto stringify
        = detail::overload([](const auto& x) { return caf::deep_to_string(x); },
                           [](const std::string& x) { return x; });
      auto str = caf::visit(stringify, val);
      auto new_val = option.parse(str);
      if (!new_val) {
        // If this fails, we try to parse the string as atom, since this the
        // only type that we cannot distinguish. Everything else is a true type
        // clash.
        new_val = option.parse("'" + str + "'");
        if (!new_val)
          return caf::make_error(ec::type_clash,
                                 "failed to parse config option",
                                 caf::deep_to_string(option.full_name()), str,
                                 "expected",
                                 caf::deep_to_string(option.type_name()));
      }
      put(content, option.full_name(), std::move(*new_val));
    }
  }
  // Try parsing all --caf.* settings. First, strip caf. prefix for the
  // CAF parser.
  for (auto& arg : caf_args)
    if (detail::starts_with(arg, "--caf."))
      arg.erase(2, 4);
  // We clear the config_file_path first so it does not use
  // caf-application.ini as fallback during actor_system_config::parse().
  config_file_path.clear();
  return actor_system_config::parse(std::move(caf_args));
}

} // namespace vast::system
