(* an unboxed record of bool, and of char *)
type t = { b : bool } [@@unboxed]
type u = U of { c : char } [@@unboxed]
