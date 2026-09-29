module type S = sig type t = A | B end
module F (X : S) : S = X
