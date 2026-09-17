module Id (X : sig type t end) = X
module A = struct type t end
module P = struct type v = Id (A).t end
