(* control: a local type written applied costs nothing *)
type 'a t = A of 'a
module type S = sig val x : int t end
let z = 1
