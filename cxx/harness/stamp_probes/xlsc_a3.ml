module type S = sig val w : int end
module type T = sig val w : string end
let f (module X : S) = X.w
let h x = let (module X : T) = x in X.w
