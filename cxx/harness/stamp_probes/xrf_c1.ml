type _ t = C : 'a -> 'a t
let f : type a. a t -> a = function C x -> x
