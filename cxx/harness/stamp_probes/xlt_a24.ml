module Id (X : sig type t end) = X
module A = struct type t end
module Z = Id (A)
type u = Id (A).t
