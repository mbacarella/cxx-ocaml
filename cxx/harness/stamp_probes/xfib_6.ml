module type S0 = sig type key end
module M0 = struct type key end
module type S1 = (S0 -> S0) -> S0
module M4 : S1 = functor (P1 : S0 -> S0) -> M0
