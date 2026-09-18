module type S0 = sig type a type b end
module type S0' = sig include S0 type c end
module type T = (S0 -> sig end) -> S0
