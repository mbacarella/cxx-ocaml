module type S0 = sig type key end
module type S0' = sig include S0 val tag : string end
module M0 = struct type key end
module M1 (P1 : S0 -> S0') = struct include P1(M0) let v = 42 end
