module type P = sig module N : sig type s val x : s end end
module F (X : P) = struct include X end
