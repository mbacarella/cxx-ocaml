module Id (X : sig type t end) = X
module A = struct type t end
type u = private Id (A).t
