(* through an abbreviation, and a closed polyvariant *)
type l = int
type t = E of l [@@unboxed]
type u = U of [ `A | `B ] [@@unboxed]
