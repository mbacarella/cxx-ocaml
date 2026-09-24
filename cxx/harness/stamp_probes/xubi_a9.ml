(* a recursive group, forward reference *)
type t = E of u [@@unboxed]
and u = A | B
