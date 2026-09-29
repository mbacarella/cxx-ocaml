module type S = sig val x : int end
module type T = sig val y : int end
let f (m : (module S)) : (module T) = m
