module Id (X : sig type t end) = X
module A = struct type t = int end
type u = Id (A).t
