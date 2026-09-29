module Id (X : sig type t end) = X
module A = struct type t end
module P : sig type u = A.t end = struct type u = Id (A).t end
type z = Id (A).t
