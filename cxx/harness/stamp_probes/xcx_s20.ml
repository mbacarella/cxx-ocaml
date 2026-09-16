type _ t = I : int t | L : 'a list t | P : ('a * 'b) t | F : ('a -> 'b) t
type 'a box = Box of 'a
type r = { u : int; v : string }
let f (type a) (x : a t) = match x with L | P -> 0 | I | F -> 1
