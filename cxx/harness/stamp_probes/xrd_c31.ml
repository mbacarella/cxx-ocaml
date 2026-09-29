module O = struct module type T = sig type s val x : s end end
module F (X : O.T) = struct include X end
