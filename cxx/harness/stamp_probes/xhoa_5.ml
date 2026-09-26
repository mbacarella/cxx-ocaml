module type S0 = sig type key end
module M0 = struct type key end
module P1 (X : S0) = struct type u = X.key let u = 0 end
module M1 (P1 : S0 -> S0) = struct module X = P1(M0) let v = 1 end
