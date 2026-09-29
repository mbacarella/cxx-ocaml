(* an annotated value instantiated twice cites the annotation's block *)
let f : int -> int = fun v -> v
let a = f 0
let b = f 1
