module Id (X : sig type t end) = (X : sig type t = X.t end)
module A = struct type t end
type u = Id (A).t
