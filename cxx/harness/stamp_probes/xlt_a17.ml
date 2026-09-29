module Id (X : sig type t end) = X
module A = struct type t end
module B = struct type t end
type u = Id (A).t
type v = Id (B).t
