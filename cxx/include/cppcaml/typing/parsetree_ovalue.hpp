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

// A whole parsetree as Pparse.write_ast marshals it: its locations go
// through the writer's location model (OValue::loc_val).
const OValue* ovalue_of_ast_structure(Structure s);
const OValue* ovalue_of_ast_signature(Signature s);
// The inverse (parsetree_of_ovalue.cpp): the parsetree input_value read
// (Pparse's binary AST files, a rewriter's output), keeping its sharing --
// one node, location record, list and string per marshaled block.  Throws
// std::runtime_error on a value of another shape.
Structure structure_of_ovalue(const OValue* v);
Signature signature_of_ovalue(const OValue* v);
// the same, decoding the marshaled value at data[off] (advanced past it)
// directly, without a generic value in between (Pparse's binary ASTs)
Structure structure_of_marshal(const std::uint8_t* data, std::size_t len, std::size_t& off);
Signature signature_of_marshal(const std::uint8_t* data, std::size_t len, std::size_t& off);

}  // namespace cppcaml::typing::parsetree
