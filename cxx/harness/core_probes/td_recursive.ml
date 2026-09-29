type t = { next : t option; v : int }
type a = A of b | Nil and b = B of a
let rec l = { next = None; v = 1 }
