module type S0 = sig type key end
module M0 = struct
  type key type v = A | B let x = 1 module N = struct type w end
end
module F (X : S0) = M0
