module N = struct module type T = sig val w : int end end
let f x = let (module X : N.T) = x in X.w
