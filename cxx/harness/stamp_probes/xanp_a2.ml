(* the same through a record PATTERN, and through a sub-tuple *)
type t = { a : int; b : string * int }
let { a; b = (c, d) } = { a = 1; b = ("", 2) }
