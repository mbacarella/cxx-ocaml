module Id (X : sig type t end) = X
module A = struct type t end
let f (x : Id (A).t) = x
type u = Id (A).t
