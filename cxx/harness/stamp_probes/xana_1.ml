module F (X : sig type t end) = struct type u = A of X.t | B end
module N = F (struct type t end)
