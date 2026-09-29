type _ t = C : < m : int; .. > -> int t | D : bool t
let f : type a. a t -> int = function D -> 0
