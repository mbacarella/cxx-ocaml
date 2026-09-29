module Id (X : sig type t end) = X
module A = struct type t end
module P : sig type u = Id (A).t end = struct type u = Id (A).t end
