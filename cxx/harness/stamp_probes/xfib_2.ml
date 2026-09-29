module type S0 = sig type key end
module M0 = struct type key module N = struct type w end end
module F (X : S0) = M0.N
