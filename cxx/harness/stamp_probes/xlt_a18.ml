module Id (X : sig type t type s end) = X
module A = struct type t type s end
type u = Id (A).t
