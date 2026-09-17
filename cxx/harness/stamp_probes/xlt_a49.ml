module Id (X : sig type t end) = X
module A = struct type t end
type v = Id (A).t
module B = A
type w = Id (B).t
