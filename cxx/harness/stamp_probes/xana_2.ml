module F (X : sig type t type s end) = struct type u = X.t type v = X.s * int end
module N = F (struct type t = int type s end)
