module type S = sig val w : int end
let h x = let (module X : S) = x in X.w
