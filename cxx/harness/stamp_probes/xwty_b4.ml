(* a three-parameter abstract type of a submodule *)
type k = Bigarray.float64_elt
type l = Bigarray.c_layout
module type S = sig val a : (float, k, l) Bigarray.Array1.t end
let z = 1
