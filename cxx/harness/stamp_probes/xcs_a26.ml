module type S = sig type t = A | B end
module F (X : S) = struct module Y = X type u = C of X.t end
