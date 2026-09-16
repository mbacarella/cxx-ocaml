type _ t = I : int t | L : 'a list t | P : ('a * 'b) t | F : ('a -> 'b) t
type 'a box = Box of 'a
type r = { u : int; v : string }
let f (type a) (x : a t) (y : a) = match x, y with L, [] -> 0 | P, (_, _) -> 1
  | F, _ -> 2 | I, 0 -> 3
