type _ t = I : int t | L : 'a list t | P : ('a * 'b) t | F : ('a -> 'b) t
type 'a box = Box of 'a
type r = { u : int; v : string }
let f (type a) (x : a t) (y : a t) = match x, y with L, L -> 0 | P, P -> 1
  | F, F -> 2 | I, I -> 3
