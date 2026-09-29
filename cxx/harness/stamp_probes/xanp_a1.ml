(* a written annotation is ONE lookup: the field read cites its block *)
type t = { a : int; b : string }
let x = { a = 42; b = "" }
let y = x.a
