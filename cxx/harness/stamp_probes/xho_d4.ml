module type S0 = sig type key end
module M0 = struct type key end
module type S0' = sig include S0 type additional end
module type S1 = (S0 -> S0') -> S0
module M1 (P1 : S0 -> S0') = struct type key end
