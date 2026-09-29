module type A = sig type t = private int end
module F (X : A) = struct include X end
