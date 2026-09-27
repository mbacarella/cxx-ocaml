// See reporters.hpp: install(), and the small modules' error_of_exn
// (Primitive, Attr_helper, Syntaxerr's Variable_in_scope, the
// Error_forward of Typemod / Typeclass).
#include "cppcaml/typing/reporters.hpp"

#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/pprintast.hpp"
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
}

}  // namespace cppcaml::typing::reporters
