module type S0 = sig val k : int end
module F (X : sig include S0 val y : int end) = struct let w = X.y end
