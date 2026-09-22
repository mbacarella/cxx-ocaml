module type S = sig type t = A | B end
module F (X : S) = struct include X end
