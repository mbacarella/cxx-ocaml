module type T = sig type s val x : s end
module type P = sig module N : T end
module F (X : P) = struct include X.N end
