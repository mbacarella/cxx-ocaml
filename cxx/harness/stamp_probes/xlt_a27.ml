module Id (X : sig type t end) = struct type t = X.t end
module A = struct type t end
type u = Id (A).t
type v = Id (A).t
