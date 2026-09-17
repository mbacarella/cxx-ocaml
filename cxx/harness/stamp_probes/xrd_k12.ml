module O = struct module type T = sig type s val x : s end end
module type P = sig module N : O.T end
module F (X : P) = struct include X.N include X.N end
