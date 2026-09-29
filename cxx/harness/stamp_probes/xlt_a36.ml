module Id (X : sig type t end) = X
module A = struct type t end
module type T = sig val v : Id (A).t end
