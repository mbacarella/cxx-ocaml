type _ t = I : int t | L : 'a list t | P : ('a * 'b) t | F : ('a -> 'b) t
type 'a box = Box of 'a
type r = { u : int; v : string }
let f (type a) (x : a t box) = match x with Box L -> 0 | Box I -> 1
