module F (X : sig type t end) = struct type u = X.t list end
module N = F (struct type a type t = a list end)
