module O = struct module type T = sig type s val x : s end end
module type P = sig module N : O.T end
module F (X : P) : sig end = struct include X.N end
