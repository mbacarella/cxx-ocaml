module type S0 = sig type key val k : int end
module type S0' = sig include S0 type additional val tag : string end
module G (X : S0') = X
