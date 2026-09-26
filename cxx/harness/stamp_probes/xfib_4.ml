module type S0 = sig type key end
module A = struct module M0 = struct type t module Q = struct type q end end end
module C = A.M0
module FC (X : S0) = C
module FQ (X : S0) = C.Q
