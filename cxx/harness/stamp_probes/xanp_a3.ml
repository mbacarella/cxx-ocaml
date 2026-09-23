(* two labels of the same written type are two lookups, not one *)
type t = { x : int; y : int }
let f { x = c } = fun () -> c
