(* an unboxed wrapper of int is immediate *)
type t = E of int [@@unboxed]
