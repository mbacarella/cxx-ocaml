module Id (X : sig type t end) = X
module A = struct type t end
let f (x : Id (A).t) : Id (A).t = x
