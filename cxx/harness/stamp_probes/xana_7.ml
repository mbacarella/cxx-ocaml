module F (X : sig type t end) = struct type u = X.t end
module N = F (struct type a = int type t = a list end)
