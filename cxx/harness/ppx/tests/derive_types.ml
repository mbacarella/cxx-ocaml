type t = A | B [@@deriving name]
type u = { f : int } [@@deriving name]
and v = int list
let names = [ name_of_t; name_of_u ]
