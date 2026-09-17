module Id (X : sig type t end) = X
module A = struct type t end
type v = Id (A).t
type w = Id (A).t list
