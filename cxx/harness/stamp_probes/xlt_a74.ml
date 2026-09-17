module Id (X : sig type t end) = X
module A = struct type t end
type u = Id (A).t
module P = struct type w = Id (A).t end
