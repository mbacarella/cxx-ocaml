module type S = sig val w : int end
module type T = sig val w : string end
let f x = let module X = (val x : S) in X.w
let h (module X : T) = X.w
