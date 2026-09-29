module Id (X : sig type t end) = X
module A = struct type t end
type 'a u = 'a * Id (A).t
