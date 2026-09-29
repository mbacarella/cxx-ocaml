module Id (X : sig type t end) = X
module A = struct type t end
module B = struct type t end
type u = Id (A).t
module P = struct type w = Id (B).t end
