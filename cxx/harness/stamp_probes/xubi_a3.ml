(* an unboxed wrapper of a constant variant *)
type c = A | B
type t = E of c [@@unboxed]
