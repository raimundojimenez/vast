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

#include "vast/directory.hpp"

#include "vast/concept/printable/vast/error.hpp"
#include "vast/concept/printable/vast/filesystem.hpp"
#include "vast/detail/assert.hpp"
#include "vast/detail/posix.hpp"
#include "vast/detail/string.hpp"

#include <caf/streambuf.hpp>

#include <fstream>
#include <iterator>

#ifdef VAST_POSIX
#  include <sys/types.h>
#endif // VAST_POSIX

namespace vast {

directory::iterator::iterator(directory* dir) : dir_{dir} {
  increment();
}

void directory::iterator::increment() {
  if (!dir_)
    return;
#ifdef VAST_POSIX
  if (!dir_->dir_) {
    dir_ = nullptr;
  } else if (auto ent = ::readdir(dir_->dir_)) {
    auto d = ent->d_name;
    VAST_ASSERT(d);
    auto dot = d[0] == '.' && d[1] == '\0';
    auto dotdot = d[0] == '.' && d[1] == '.' && d[2] == '\0';
    if (dot || dotdot)
      increment();
    else
      current_ = dir_->path_ / d;
  } else {
    dir_ = nullptr;
  }
#endif
}

const path& directory::iterator::dereference() const {
  return current_;
}

bool directory::iterator::equals(const iterator& other) const {
  return dir_ == other.dir_;
}

directory::directory(vast::path p)
  : path_{std::move(p)}, dir_{::opendir(path_.str().data())} {
}

directory::~directory() {
#ifdef VAST_POSIX
  if (dir_)
    ::closedir(dir_);
#endif
}

directory::iterator directory::begin() {
  return iterator{this};
}

directory::iterator directory::end() const {
  return {};
}

const path& directory::path() const {
  return path_;
}

} // namespace vast
