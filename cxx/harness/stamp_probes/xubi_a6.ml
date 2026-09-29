(* an unboxed of an unboxed *)
type t = E of int [@@unboxed]
type u = U of t [@@unboxed]
