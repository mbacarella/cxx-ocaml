type _ t = I : int t | B : bool t
let f : type a. a t -> a = function I -> 1
