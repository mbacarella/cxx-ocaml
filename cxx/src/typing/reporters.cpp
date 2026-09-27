// See reporters.hpp: install(), and the small modules' error_of_exn
// (Primitive, Attr_helper, Syntaxerr's Variable_in_scope, the
// Error_forward of Typemod / Typeclass).
#include "cppcaml/typing/reporters.hpp"

#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/pprintast.hpp"
#include "cppcaml/typing/bytepackager.hpp"
#include "cppcaml/typing/cmi_format.hpp"
#include "cppcaml/typing/persistent_env.hpp"
#include "cppcaml/typing/primitive.hpp"
#include "cppcaml/typing/typeclass.hpp"
#include "cppcaml/typing/typecore.hpp"
#include "cppcaml/typing/typemod.hpp"

namespace cppcaml::typing::reporters {

namespace fd = format_doc;
using fd::Formatter;
using fd::fprintf;
using location::Report;
using misc::style::code_str;

Report error_of_extension(const parsetree::Extension* ext);

void register_misc() {
  location::register_error_of_exn([](std::exception_ptr ep) -> std::optional<Report> {
    try {
      std::rethrow_exception(ep);
    } catch (const primitive::Error& e) {
      using K = primitive::Error::Kind;
      K k = e.kind;
      return location::error_of_printer(e.loc, [k](Formatter& ppf) {
        switch (k) {
          case K::Old_style_float_with_native_repr_attribute:
            fprintf(ppf, "Cannot use %a in conjunction with %a/%a.", code_str("float"), code_str("[@unboxed]"),
                    code_str("[@untagged]"));
            break;
          case K::Old_style_noalloc_with_noalloc_attribute:
            fprintf(ppf, "Cannot use %a in conjunction with %a.", code_str("noalloc"), code_str("[@@noalloc]"));
            break;
          case K::No_native_primitive_with_repr_attribute:
            fprintf(ppf,
                    "@[The native code version of the primitive is mandatory@ when attributes %a or %a are "
                    "present.@]",
                    code_str("[@untagged]"), code_str("[@unboxed]"));
            break;
        }
      });
    } catch (const attr_helper::Error& e) {
      std::string name(e.name);
      bool multiple = e.kind == attr_helper::Error::Kind::Multiple_attributes;
      return location::error_of_printer(e.loc, [name, multiple](Formatter& ppf) {
        if (multiple)
          fprintf(ppf, "Too many %a attributes", code_str(name));
        else
          fprintf(ppf, "Attribute %a does not accept a payload", code_str(name));
      });
    } catch (const typecore::VariableInScope& e) {
      return location::errorf(e.loc, "In this scoped type, variable %a is reserved for the local type %a.",
                              misc::style::code(pprintast::tyvar, e.name), code_str(e.name));
    } catch (const persistent_env::Error& e) {
      using K = persistent_env::Error::Kind;
      K k = e.kind;
      std::string a = e.a, b = e.b, c = e.c;
      return location::error_of_printer_file([=](Formatter& ppf) {
        auto qf = [](const std::string& f) { return [f](Formatter& ff) { location::doc::quoted_filename(ff, f); }; };
        switch (k) {
          case K::Illegal_renaming:
            fprintf(ppf, "Wrong file naming: %a@ contains the compiled interface for@ %a when %a was expected", qf(c),
                    code_str(b), code_str(a));
            break;
          case K::Inconsistent_import:
            fprintf(ppf, "@[<hov>The files %a@ and %a@ make inconsistent assumptions@ over interface %a@]", qf(b),
                    qf(c), code_str(a));
            break;
          case K::Need_recursive_types:
            fprintf(ppf, "@[<hov>Invalid import of %a, which uses recursive types.@ The compilation flag %a is required@]",
                    code_str(a), code_str("-rectypes"));
            break;
        }
      });
    } catch (const cmi_format::Error& e) {
      using K = cmi_format::Error::Kind;
      K k = e.kind;
      std::string f = e.filename, on = e.older_newer;
      return location::error_of_printer_file([=](Formatter& ppf) {
        auto qf = [f](Formatter& ff) { location::doc::quoted_filename(ff, f); };
        switch (k) {
          case K::Not_an_interface: fprintf(ppf, "%a@ is not a compiled interface", qf); break;
          case K::Wrong_version_interface:
            fprintf(ppf,
                    "%a@ is not a compiled interface for this version of OCaml.@.It seems to be for %s version of "
                    "OCaml.",
                    qf, on);
            break;
          case K::Corrupted_interface: fprintf(ppf, "Corrupted compiled interface@ %a", qf); break;
        }
      });
    } catch (const bytepackager::Error& e) {
      using K = bytepackager::Error::Kind;
      K k = e.kind;
      std::string file = e.file, name = e.name, id = e.id, auth = e.auth;
      return location::error_of_printer_file([=](Formatter& ppf) {
        auto qf = [](const std::string& f) { return [f](Formatter& ff) { location::doc::quoted_filename(ff, f); }; };
        switch (k) {
          case K::Forward_reference:
            fprintf(ppf, "Forward reference to %a in file %a", code_str(name), qf(file));
            break;
          case K::Multiple_definition: fprintf(ppf, "File %a redefines %a", qf(file), code_str(name)); break;
          case K::Not_an_object_file: fprintf(ppf, "%a is not a bytecode object file", qf(file)); break;
          case K::Illegal_renaming:
            fprintf(ppf, "Wrong file naming: %a@ contains the code for@ %a when %a was expected", qf(file),
                    code_str(name), code_str(id));
            break;
          case K::File_not_found: fprintf(ppf, "File %a not found", code_str(file)); break;
          case K::Inconsistent_import:
            // Bytelink.report_error's
            fprintf(ppf, "@[<hov>Files %a@ and %a@ make inconsistent assumptions over interface %a@]", qf(file),
                    qf(auth), code_str(name));
            break;
        }
      });
    } catch (const typemod::ErrorForward& e) {
      return error_of_extension(e.ext);
    } catch (const typeclass::ErrorForward& e) {
      return error_of_extension(e.ext);
    } catch (...) {
    }
    return std::nullopt;
  });
}

void install() {
  static bool done = false;
  if (done) return;
  done = true;
  register_misc();
  register_env();
  register_typetexp();
  register_typecore();
  register_typedecl();
  register_typemod();
  register_includemod();
  register_typeclass();
}

}  // namespace cppcaml::typing::reporters
