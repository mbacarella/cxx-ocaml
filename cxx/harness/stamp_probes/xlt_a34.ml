module Id (X : sig type t end) = X
module A = struct type t end
type u = Id (A).t
let f (x : Id (A).t) = x
