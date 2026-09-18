module type S0 = sig type z1 type z2 end
module type S0' = sig include S0 type additional end
module type T = S0 -> S0'
module type S1 = (S0 -> S0') -> S0
module type S1t = T -> S0
module F (X : S0 -> S0') : sig end = struct end
