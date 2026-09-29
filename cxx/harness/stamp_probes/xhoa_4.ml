module type S0 = sig type key end
module M0 = struct type key end
module M1 (P1 : functor (X : S0) -> sig type t = X.key end) =
  struct module X = P1(M0) let v = 1 end
