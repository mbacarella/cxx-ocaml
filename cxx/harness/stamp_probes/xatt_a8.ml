(* a variant and a record keep an empty-payload attribute *)
type t = A | B [@@warn_on_literal_pattern]
type r = { a : int } [@@boxed]
