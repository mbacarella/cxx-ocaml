module type T = sig module N : sig type s val x : s end end
module F (X : T) = struct type u = X.N.s end
