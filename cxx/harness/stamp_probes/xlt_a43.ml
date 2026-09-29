module Id (X : sig type t end) = X
module A = struct type t end
exception E of Id (A).t
