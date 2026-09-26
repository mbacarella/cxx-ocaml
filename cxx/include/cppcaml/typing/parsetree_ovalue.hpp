// The OCaml runtime representation of Parsetree values (parsetree.mli,
// asttypes.mli, longident.mli, location.mli), as the generic OValue the
// cmi reader decodes marshaled data into: constant constructors are ints,
// non-constant ones blocks tagged in declaration order, records blocks in
// field order, lists cons blocks, options None = 0 / Some = block 0.
// Used for the attribute payloads Types records carry (attr_payload).
#pragma once

#include "cppcaml/typing/parsetree.hpp"

namespace cppcaml::typing::parsetree {

const OValue* ovalue_of_location(const Location& l);
const OValue* ovalue_of_longident(Longident::t lid);
// docstring: an ocaml.doc / ocaml.text attribute, built by Docstrings
const OValue* ovalue_of_payload(const Payload& p, bool docstring);
const OValue* ovalue_of_attribute(const Attribute* a);
const OValue* ovalue_of_structure(Structure s);
const OValue* ovalue_of_signature(Signature s);
const OValue* ovalue_of_core_type(const CoreType* t);
const OValue* ovalue_of_expression(const Expression* e);
const OValue* ovalue_of_pattern(const Pattern* p);

}  // namespace cppcaml::typing::parsetree
