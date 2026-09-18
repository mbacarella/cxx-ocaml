module type S0 = sig type z1 type z2 end module M0 = struct type z1 type z2 end
module type S0' = sig include S0 type additional end module type T = S0 -> S0'
module type S1 = (S0 -> S0') -> S0
module M1 : S1 = functor (P1 : S0 -> S0') -> P1(M0) module Gn (_ : S0) = struct
type z1 type z2 type additional end module G2n (_ : S0) (_ : S0) = struct
type z1 type z2 type additional end module G2 (X : S0) (_ : S0) = struct type z1
type z2 type additional end module F (P1 : S0 -> S0 -> S0') = P1(M0)
