module Id (X : sig type t end) : sig type t end = X
module A = struct type t end
type u = Id (A).t
