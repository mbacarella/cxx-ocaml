module type S0 = sig type z1 type z2 end module M0 = struct type z1 type z2 end
module type S0' = sig include S0 type additional end module type T = S0 -> S0'
module type S1 = (S0 -> S0') -> S0 module type S1t = T -> S0
module M1 : S1 = functor (P1 : S0 -> S0') -> P1(M0)
module M1t : S1t = functor (P1 : T) -> P1(M0)
module M1f (P1 : S0 -> S0') = P1(M0) module M1n (_ : S0 -> S0') = M0
module G (X : S0) = struct include X type additional end
module Gn (_ : S0) = struct type z1 type z2 type additional end module A = G(M0)
