(* an unboxed wrapper keeps its attribute *)
type 'a t = A of 'a [@@unboxed]
type r = { x : int } [@@ocaml.unboxed]
