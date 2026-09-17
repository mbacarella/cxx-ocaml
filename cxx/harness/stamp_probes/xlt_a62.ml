module Id (X : sig type t end) = X
module A = struct type t end
module P : sig type u = A.t end = struct type v = Id (A).t type u = v end
