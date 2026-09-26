module type S0 = sig type key end
module type SB = sig type k module Z : sig type z end end
module B : SB = struct type k module Z = struct type z end end
module FB (X : S0) = B
module FBZ (X : S0) = B.Z
