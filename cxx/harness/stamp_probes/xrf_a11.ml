type _ t = C : (< m : int; .. > as 'r) -> 'r t
let f : type a. a t -> int = function C _ -> 0
