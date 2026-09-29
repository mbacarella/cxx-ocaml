module Id (X : sig type t end) = X
module A = struct type t end
module P : sig type u end = struct type u = Id (A).t type w = Id (A).t end
