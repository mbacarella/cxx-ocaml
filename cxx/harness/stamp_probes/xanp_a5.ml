(* a constructor argument read travels the same way *)
type t = A of int | B of string
let f = function A n -> n | B _ -> 0
