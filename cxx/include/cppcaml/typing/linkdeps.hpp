// Port of utils/linkdeps.ml: the compilation units a link requires and
// provides, and its missing / badly ordered / duplicated units.  The tables
// are OCaml Hashtbls (hashtbl.hpp): the order of an error report's entries
// is their iteration order.
#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cppcaml/typing/format_doc.hpp"

namespace cppcaml::typing::linkdeps {

struct CompunitAndSource {
  std::string compunit;
  std::string filename;
  bool operator==(const CompunitAndSource& o) const { return compunit == o.compunit && filename == o.filename; }
  bool operator<(const CompunitAndSource& o) const {  // compare
    int c = compunit.compare(o.compunit);
    return c != 0 ? c < 0 : filename.compare(o.filename) < 0;
  }
};
using Refs = std::set<CompunitAndSource>;

struct Error {
  enum class Kind { Missing_implementations, Wrong_link_order, Multiple_definitions } kind;
  // Missing_implementations: (compunit * compunit_and_source list) list
  std::vector<std::pair<std::string, std::vector<CompunitAndSource>>> missing;
  // Wrong_link_order: (compunit_and_source * compunit_and_source list) list
  std::vector<std::pair<CompunitAndSource, std::vector<CompunitAndSource>>> wrong_order;
  // Multiple_definitions: (compunit * filename list) list
  std::vector<std::pair<std::string, std::vector<std::string>>> multiple;
};

class T {
 public:
  explicit T(bool complete);
  ~T();
  // required t compunit
  bool required(const std::string& compunit) const;
  // add t ~filename ~compunit ~provides ~requires
  void add(const std::string& filename, const std::string& compunit, const std::vector<std::string>& provides,
           const std::vector<std::string>& requires_);
  std::optional<Error> check() const;

 private:
  struct Tables;
  bool complete_;
  std::unique_ptr<Tables> t_;
};

// report_error_doc ~print_filename ppf e
void report_error_doc(format_doc::Formatter& ppf, const Error& e);

}  // namespace cppcaml::typing::linkdeps
