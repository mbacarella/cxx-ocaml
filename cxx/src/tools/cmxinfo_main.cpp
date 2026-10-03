// Test tool: ocamlobjinfo's print_cmx_infos for a non-flambda .cmx (the
// oracle for the .cmx reader: `ocamlobjinfo foo.cmx`).
#include <cstdio>
#include <string>

#include "cppcaml/blake2.hpp"
#include "cppcaml/typing/cmx_format.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/export_info.hpp"
#include "cppcaml/typing/flambda_ids.hpp"
#include "cppcaml/typing/format.hpp"
#include "cppcaml/typing/printclambda.hpp"

using namespace cppcaml::typing;

namespace {
void print_name_crc(const std::pair<std::string_view, std::optional<std::string>>& e) {
  std::string crc = e.second ? cppcaml::blake2::to_hex(*e.second) : std::string(32, '-');
  std::printf("\t%s\t%.*s\n", crc.c_str(), static_cast<int>(e.first.size()), e.first.data());
}
}  // namespace

int main(int argc, char** argv) {
  // --roundtrip in.cmxa out.cmxa: read a library and write it again (the
  // oracle for the .cmxa writer: the bytes are ocamlopt -a's)
  if (argc == 4 && std::string(argv[1]) == "--roundtrip") {
    std::string bytes = cmx_format::write_library_info(cmx_format::read_library_info(argv[2]));
    std::FILE* fp = std::fopen(argv[3], "wb");
    if (!fp) return 2;
    std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
    return 0;
  }
  for (int i = 1; i < argc; ++i) {
    std::printf("File %s\n", argv[i]);
    try {
      auto [ui, crc] = cmx_format::read_unit_info(argv[i]);
      std::printf("Name: %.*s\n", static_cast<int>(ui->ui_name.size()), ui->ui_name.data());
      std::printf("CRC of implementation: %s\n", cppcaml::blake2::to_hex(crc).c_str());
      std::printf("Globals defined:\n");
      for (auto d : ui->ui_defines) std::printf("\t%.*s\n", static_cast<int>(d.size()), d.data());
      std::printf("Interfaces imported:\n");
      for (auto& e : ui->ui_imports_cmi) print_name_crc(e);
      std::printf("Implementations imported:\n");
      for (auto& e : ui->ui_imports_cmx) print_name_crc(e);
      if (ui->ui_flambda_export_info) {
        // Export_info.print_approx / print_functions, with the unit as
        // ocamlobjinfo sets it up
        std::printf("Flambda export information:\n");
        compilation_unit::t cu =
            compilation_unit::create(Ident::create_persistent(ui->ui_name), std::string_view("__dummy__"));
        compilation_unit::set_current(cu);
        std::vector<symbol::t> root_symbols;
        for (auto d : ui->ui_defines)
          root_symbols.push_back(symbol::of_global_linkage(cu, zstr("caml" + std::string(d))));
        format::Formatter ppf;
        format::fprintf(ppf, "approximations@ %a@.@.",
                        [&](format::Formatter& f) { export_info::print_approx(f, ui->ui_flambda_export_info, root_symbols); });
        format::fprintf(ppf, "functions@ %a@.@.",
                        [&](format::Formatter& f) { export_info::print_functions(f, ui->ui_flambda_export_info); });
        std::fwrite(ppf.contents().data(), 1, ppf.contents().size(), stdout);
      } else {
        std::printf("Clambda approximation:\n");
        format::Formatter ppf;
        ppf.print_string("  ");
        printclambda::approx(ppf, ui->ui_export_info);
        ppf.print_newline();
        std::fputs(ppf.contents().c_str(), stdout);
      }
      auto ints = [](const char* what, const std::vector<long>& v) {
        std::printf("%s", what);
        for (long n : v) std::printf(" %ld", n);
        std::printf("\n");
      };
      ints("Currying functions:", ui->ui_curry_fun);
      ints("Apply functions:", ui->ui_apply_fun);
      ints("Send functions:", ui->ui_send_fun);
      std::printf("Force link: %s\n", ui->ui_force_link ? "YES" : "no");
      if (ui->ui_for_pack)
        std::printf("For pack: YES: %.*s\n", static_cast<int>(ui->ui_for_pack->size()), ui->ui_for_pack->data());
      else
        std::printf("For pack: no\n");
      std::printf("Requires caml_standard_library_nat: %s\n", ui->ui_need_stdlib ? "YES" : "no");
    } catch (const cmx_format::Error& e) {
      std::fprintf(stderr, "%s: %s\n", argv[i],
                   e.kind == cmx_format::Error::Kind::Not_a_unit_info ? "not a unit info" : "corrupted");
      return 2;
    }
  }
  return 0;
}
