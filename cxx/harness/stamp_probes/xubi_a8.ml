(* known miss: an instance of a parameterised unboxed type *)
type 'a w = W of 'a [@@unboxed]
type t = T of int w [@@unboxed]
