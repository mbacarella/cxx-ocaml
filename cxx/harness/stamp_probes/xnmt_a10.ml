module type S = sig val f : int -> int end
module A : S = struct let f x = x + 1 end
let a : int = 1
let b = A.f a
