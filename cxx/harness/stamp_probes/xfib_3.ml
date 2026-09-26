module type S0 = sig type key end
module M0 = struct type key end
module F (X : S0) (Y : S0) = M0
