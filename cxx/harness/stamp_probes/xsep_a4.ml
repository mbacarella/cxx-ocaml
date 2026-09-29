(* an unboxed constructor checks its argument at Sep *)
type 'a t = A of 'a [@@unboxed]
